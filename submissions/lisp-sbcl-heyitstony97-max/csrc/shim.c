/* The only C in this entry: SQLite itself (the pinned, unmodified amalgamation, #included below so
 * the compiler sees one translation unit), a few open/prepare helpers that run once at boot, and
 * the low-priority helper thread.
 *
 * The helper thread is plain C and never touches the Lisp heap. SBCL's GC stops every thread it
 * knows about; this one is invisible to it, so a collection never waits for (or runs) a
 * checkpoint. It owns everything that is not urgent:
 *   - WAL checkpoints (its own SQLite connection, PASSIVE: readers and writers never wait for it),
 *     so the event loop never waits for a checkpoint's fsyncs (see "checkpoints" below),
 *   - the memory monitor (/proc/meminfo every 200 ms into a word the event loop reads),
 *   - stats logging (the event loop writes its counters into td_stats; this thread prints them).
 * It runs at nice 19, so on the one vCPU it gets about 1 % of the CPU while the loop is busy and
 * the rest whenever the loop waits. */
#define _GNU_SOURCE
#include "sqlite3.c"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>

static sqlite3 *main_db;   /* the event loop's connection (set by shim_open) */

static int run(sqlite3 *db, const char *sql, char *err, int n) {
  char *msg = 0;
  if (sqlite3_exec(db, sql, 0, 0, &msg) != SQLITE_OK) {
    snprintf(err, n, "%s: %s", sql, msg ? msg : "error");
    sqlite3_free(msg);
    return -1;
  }
  return 0;
}

/* ---- checkpoints ---------------------------------------------------------------------------
 * SQLite's automatic checkpoint runs inside the COMMIT that crosses its frame limit, on the thread
 * that committed: here that was the event loop, which then copied the whole log back and waited for
 * two fdatasyncs with every connection queued behind it (measured: 250-470 ms every ~11 s at 20k
 * req/s). So automatic checkpoints are off, a WAL hook counts the frames, and the helper thread does
 * the copying and the syncing:
 *   1. past CKPT_FRAMES frames the loop asks the thread for a PASSIVE checkpoint (the bulk);
 *   2. the log only restarts once it is copied completely, and under steady writes a checkpoint
 *      never catches the frames written while it ran. So when the bulk is done the loop holds its
 *      write pulse (reads go on) and the thread checkpoints once more (the tail);
 *   3. when the tail copied everything the loop ends its read transaction, so the next one starts
 *      on a fully copied log and the next write restarts the log at its beginning.
 * A hold lasts at most HOLD_MAX_MS; if the thread is slower, writes go on and the next round tries
 * again. Past CKPT_FORCE frames (the thread never caught up) the loop checkpoints by itself, as a
 * last resort. The policy follows rust-iouring-kensac's (credited in the README); no code copied. */
#define CKPT_FRAMES_DEFAULT 4096
#define CKPT_FORCE_DEFAULT 65536
#define HOLD_MAX_MS 250

enum { CK_IDLE, CK_BULK, CK_BULK_DONE, CK_TAIL, CK_TAIL_DONE };
static int ckpt_frames = CKPT_FRAMES_DEFAULT;
static int ckpt_force = CKPT_FORCE_DEFAULT;
static int ckpt_delay_ms;  /* testing aid (CKPT_DELAY_MS): the thread sleeps before each checkpoint, like a slow disk */
static int wal_frames;     /* frames in the log after the loop's last commit (loop thread only) */
static unsigned commit_seq; /* commits that added frames (loop thread only) */
static unsigned ck_seq0;   /* commit_seq when the running checkpoint was asked for */
static int writes_held;    /* loop thread only */
static struct timespec hold_t0;
static int ck_state;       /* written under ck_mu; the loop polls it with an atomic load */
static int ck_full;        /* the thread's last checkpoint left nothing to copy (set before *_DONE) */
static pthread_mutex_t ck_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cv;

static int wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name;
  wal_frames = frames;
  commit_seq++;
  return SQLITE_OK;
}

/* ---- one write per commit ------------------------------------------------------------------
 * SQLite writes a transaction's WAL frames one after another, each as two writes (its 24-byte
 * header, then the page): two pwrite calls per frame, thousands a second under load. This is a VFS
 * over unix-excl (SQLite's public VFS interface; the pattern of its own vfstrace and multiplex
 * shims) that changes one thing: on a WAL file, frame writes that continue where the last one
 * ended are collected in a buffer, and the buffer goes to the file when the commit frame's page
 * arrives (a frame header whose database-size field is non-zero marks the commit frame), inside
 * SQLite's own xWrite call. So the commit is in the kernel before SQLite publishes it in the WAL
 * index and before COMMIT returns, exactly as without the buffer, and the buffer is empty whenever
 * SQLite is not inside sqlite3WalFrames. Every other write, and every read, sync, truncate, size
 * query and close of that file, writes the buffer first. Reads, locks, the shared-memory methods
 * and the database file are unix-excl's.
 * OFF unless WALBUF=1: it cuts pwrite calls from 0.24 to 0.03 per request, but no CPU difference
 * could be measured on the test rig (README, M9), so plain unix-excl stays the default.
 * The idea is c-iouring-pavlyhalim's and fsharp-uring-delneg's (credited in the README). */
#define WB_CAP (1 << 20)      /* about 250 frames; a larger commit goes out in several pieces */
#define WB_CHUNK 126976       /* the unix VFS hands at most 131,071 bytes to one pwrite */

typedef struct WbFile {
  sqlite3_file base;
  int is_wal;
  int len;                    /* bytes buffered */
  int expect_page;            /* the last buffered write was a frame header: its page comes next */
  int commit_next;            /* ...and it was a commit frame's header: flush after the page */
  sqlite3_int64 off;          /* file offset of buf[0] */
  unsigned char *buf;         /* WB_CAP bytes, allocated at the first buffered write */
} WbFile;                     /* the real (unix-excl) file follows */
#define WB_REAL(f) ((sqlite3_file *)((WbFile *)(f) + 1))

static sqlite3_vfs wb_vfs;
static sqlite3_vfs *wb_real;
static int wb_pgsz;           /* page size of the database; 0 until known: no buffering */
static const char *db_vfs = "unix-excl";

static int wb_flush(WbFile *f) {
  sqlite3_file *r = WB_REAL(f);
  int rc = SQLITE_OK, done = 0;
  while (done < f->len && rc == SQLITE_OK) {
    int n = f->len - done;
    if (n > WB_CHUNK) n = WB_CHUNK;
    rc = r->pMethods->xWrite(r, f->buf + done, n, f->off + done);
    done += n;
  }
  f->len = 0;
  f->expect_page = 0;
  f->commit_next = 0;
  return rc;
}

static int wb_write(sqlite3_file *p, const void *z, int amt, sqlite3_int64 off) {
  WbFile *f = (WbFile *)p;
  sqlite3_file *r = WB_REAL(f);
  if (!f->is_wal || wb_pgsz == 0) return r->pMethods->xWrite(r, z, amt, off);
  if (f->len) {
    if (off == f->off + f->len && amt == (f->expect_page ? wb_pgsz : 24) && f->len + amt <= WB_CAP) {
      memcpy(f->buf + f->len, z, (size_t)amt);
      f->len += amt;
      if (f->expect_page) {
        f->expect_page = 0;
        if (f->commit_next) return wb_flush(f);     /* the commit frame is complete: to the kernel now */
      } else {
        const unsigned char *h = (const unsigned char *)z;
        f->expect_page = 1;
        f->commit_next = (h[4] | h[5] | h[6] | h[7]) != 0;
      }
      return SQLITE_OK;
    }
    int rc = wb_flush(f);                           /* anything else: what is buffered goes first */
    if (rc != SQLITE_OK) return rc;
  }
  /* nothing buffered: start a buffer only at a frame header on a frame boundary */
  if (amt == 24 && off >= 32 && (off - 32) % (24 + wb_pgsz) == 0) {
    if (!f->buf) f->buf = (unsigned char *)sqlite3_malloc(WB_CAP);
    if (f->buf) {
      const unsigned char *h = (const unsigned char *)z;
      memcpy(f->buf, z, 24);
      f->off = off;
      f->len = 24;
      f->expect_page = 1;
      f->commit_next = (h[4] | h[5] | h[6] | h[7]) != 0;
      return SQLITE_OK;
    }
  }
  return r->pMethods->xWrite(r, z, amt, off);
}

#define WB_SYNCED(f) do { if ((f)->len) { int rc_ = wb_flush(f); if (rc_ != SQLITE_OK) return rc_; } } while (0)

static int wb_close(sqlite3_file *p) {
  WbFile *f = (WbFile *)p;
  sqlite3_file *r = WB_REAL(f);
  int rc1 = f->len ? wb_flush(f) : SQLITE_OK;
  sqlite3_free(f->buf);
  f->buf = 0;
  int rc2 = r->pMethods->xClose(r);
  return rc1 != SQLITE_OK ? rc1 : rc2;
}
static int wb_read(sqlite3_file *p, void *z, int amt, sqlite3_int64 off) {
  WbFile *f = (WbFile *)p; WB_SYNCED(f);
  return WB_REAL(f)->pMethods->xRead(WB_REAL(f), z, amt, off);
}
static int wb_truncate(sqlite3_file *p, sqlite3_int64 size) {
  WbFile *f = (WbFile *)p; WB_SYNCED(f);
  return WB_REAL(f)->pMethods->xTruncate(WB_REAL(f), size);
}
static int wb_sync(sqlite3_file *p, int flags) {
  WbFile *f = (WbFile *)p; WB_SYNCED(f);
  return WB_REAL(f)->pMethods->xSync(WB_REAL(f), flags);
}
static int wb_filesize(sqlite3_file *p, sqlite3_int64 *size) {
  WbFile *f = (WbFile *)p; WB_SYNCED(f);
  return WB_REAL(f)->pMethods->xFileSize(WB_REAL(f), size);
}
static int wb_lock(sqlite3_file *p, int l) { return WB_REAL(p)->pMethods->xLock(WB_REAL(p), l); }
static int wb_unlock(sqlite3_file *p, int l) { return WB_REAL(p)->pMethods->xUnlock(WB_REAL(p), l); }
static int wb_check(sqlite3_file *p, int *out) { return WB_REAL(p)->pMethods->xCheckReservedLock(WB_REAL(p), out); }
static int wb_fcntl(sqlite3_file *p, int op, void *arg) {
  WbFile *f = (WbFile *)p; WB_SYNCED(f);
  return WB_REAL(f)->pMethods->xFileControl(WB_REAL(f), op, arg);
}
static int wb_sector(sqlite3_file *p) { return WB_REAL(p)->pMethods->xSectorSize(WB_REAL(p)); }
static int wb_devchar(sqlite3_file *p) { return WB_REAL(p)->pMethods->xDeviceCharacteristics(WB_REAL(p)); }
static int wb_shmmap(sqlite3_file *p, int pg, int sz, int ext, void volatile **pp) {
  return WB_REAL(p)->pMethods->xShmMap(WB_REAL(p), pg, sz, ext, pp);
}
static int wb_shmlock(sqlite3_file *p, int off, int n, int flags) { return WB_REAL(p)->pMethods->xShmLock(WB_REAL(p), off, n, flags); }
static void wb_shmbarrier(sqlite3_file *p) { WB_REAL(p)->pMethods->xShmBarrier(WB_REAL(p)); }
static int wb_shmunmap(sqlite3_file *p, int del) { return WB_REAL(p)->pMethods->xShmUnmap(WB_REAL(p), del); }
static int wb_fetch(sqlite3_file *p, sqlite3_int64 off, int amt, void **pp) { return WB_REAL(p)->pMethods->xFetch(WB_REAL(p), off, amt, pp); }
static int wb_unfetch(sqlite3_file *p, sqlite3_int64 off, void *pg) { return WB_REAL(p)->pMethods->xUnfetch(WB_REAL(p), off, pg); }

static const sqlite3_io_methods wb_methods = {
  3, wb_close, wb_read, wb_write, wb_truncate, wb_sync, wb_filesize, wb_lock, wb_unlock, wb_check,
  wb_fcntl, wb_sector, wb_devchar, wb_shmmap, wb_shmlock, wb_shmbarrier, wb_shmunmap, wb_fetch, wb_unfetch
};

/* The real VFS's own methods get the real VFS: unix-excl recognises itself by name in xOpen. */
static int wb_open(sqlite3_vfs *v, sqlite3_filename name, sqlite3_file *p, int flags, int *out) {
  (void)v;
  WbFile *f = (WbFile *)p;
  memset(f, 0, sizeof *f);
  int rc = wb_real->xOpen(wb_real, name, WB_REAL(f), flags, out);
  if (WB_REAL(f)->pMethods) {           /* opened (or failed in a way that still wants xClose) */
    f->base.pMethods = &wb_methods;
    f->is_wal = rc == SQLITE_OK && (flags & SQLITE_OPEN_WAL) != 0;
  }
  return rc;
}
static int wb_delete(sqlite3_vfs *v, const char *name, int dirsync) { (void)v; return wb_real->xDelete(wb_real, name, dirsync); }
static int wb_access(sqlite3_vfs *v, const char *name, int flags, int *out) { (void)v; return wb_real->xAccess(wb_real, name, flags, out); }
static int wb_fullpath(sqlite3_vfs *v, const char *name, int n, char *out) { (void)v; return wb_real->xFullPathname(wb_real, name, n, out); }

/* Registers the VFS (once) and selects it, when WALBUF=1. */
static void wb_setup(void) {
  const char *e = getenv("WALBUF");
  if (wb_real || !e || e[0] != '1') return;
  sqlite3_vfs *real = sqlite3_vfs_find("unix-excl");
  if (!real || real->iVersion < 1) return;
  wb_vfs = *real;                        /* randomness, sleep, time, dlopen, syscalls: unix-excl's */
  wb_vfs.pNext = 0;
  wb_vfs.zName = "unix-excl-walbuf";
  wb_vfs.szOsFile = (int)sizeof(WbFile) + real->szOsFile;
  wb_vfs.xOpen = wb_open;
  wb_vfs.xDelete = wb_delete;
  wb_vfs.xAccess = wb_access;
  wb_vfs.xFullPathname = wb_fullpath;
  wb_real = real;
  if (sqlite3_vfs_register(&wb_vfs, 0) == SQLITE_OK) db_vfs = wb_vfs.zName;
  else wb_real = 0;
}

/* Opens the database through the unix-excl VFS (this process takes an exclusive lock and keeps it,
 * and the WAL index lives in heap memory shared by the process's two connections) and applies the
 * pragmas. NULL on failure, reason in err. */
sqlite3 *shim_open(const char *path, char *err, int n) {
  sqlite3 *db = 0;
  int rc = sqlite3_initialize();
  if (rc != SQLITE_OK) { snprintf(err, n, "sqlite3_initialize: %d", rc); return 0; }
  wb_setup();
  rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, db_vfs);
  if (rc != SQLITE_OK) {
    snprintf(err, n, "open %s: %s", path, db ? sqlite3_errmsg(db) : "out of memory");
    if (db) sqlite3_close(db);
    return 0;
  }
  sqlite3_busy_timeout(db, 5000);
  if (run(db, "PRAGMA journal_mode = WAL", err, n) || run(db, "PRAGMA synchronous = NORMAL", err, n) ||
      run(db, "PRAGMA temp_store = MEMORY", err, n) || run(db, "PRAGMA cache_size = -65536", err, n) ||
      run(db, "PRAGMA mmap_size = 0", err, n) || run(db, "PRAGMA wal_autocheckpoint = 0", err, n) ||
      run(db, "SELECT count(*) FROM sqlite_schema", err, n)) {
    sqlite3_close(db);
    return 0;
  }
  sqlite3_wal_hook(db, wal_hook, 0);   /* after the pragma: wal_autocheckpoint installs a hook of its own */
  if (wb_real) {                       /* the WAL buffer needs the page size to recognise frame boundaries */
    sqlite3_stmt *st = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA page_size", -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
      wb_pgsz = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  fprintf(stderr, "sqlite vfs: %s%s\n", db_vfs, wb_real && wb_pgsz ? " (one write per commit)" : "");
  main_db = db;
  return db;
}

/* A statement kept for the life of the process. */
sqlite3_stmt *shim_prepare(sqlite3 *db, const char *sql, char *err, int n) {
  sqlite3_stmt *st = 0;
  if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, 0) != SQLITE_OK) {
    snprintf(err, n, "prepare: %s", sqlite3_errmsg(db));
    return 0;
  }
  return st;
}

/* ---- B-tree cursor reads (PR #153: the organiser allowed reading through SQLite's own B-tree) ---
 *
 * GET /feed, GET /posts/:id and the like handler's existence check read SQLite's B-tree cursors
 * inside the pass's read transaction instead of running the bytecode engine. The cursors are thin
 * wrappers over the unmodified amalgamation's internals (this file #includes sqlite3.c). Record
 * decoding happens in Lisp (src/btree.lisp). Writes stay on prepared SQL.
 *
 * td_bt_init checks the schema the decoders assume and resolves the four root pages once; on any
 * surprise it returns -1 with the reason and the server uses the prepared-SQL path instead. Nothing
 * here depends on row data: only on the schema. Every request seeks and reads its rows live. */
static Btree *bt;
static Pgno root_posts, root_users, root_feed_idx, root_likes_idx;
static KeyInfo *ki_feed, *ki_likes;
#define NCUR 4
static BtCursor *cur[NCUR];
static int cur_open;
static uint8_t *spill;     /* records that spill onto overflow pages are copied here */
static uint32_t spill_cap;

static Pgno bt_rootpage(const char *name) {
  sqlite3_stmt *s;
  Pgno r = 0;
  if (sqlite3_prepare_v2(main_db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, 0)) return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return r;
}

/* Comma-joined result of a one-column query equals `want`. */
static int bt_layout(const char *sql, const char *want) {
  sqlite3_stmt *s;
  char got[512];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(main_db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
    if (n >= sizeof got) break;
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

static KeyInfo *bt_keyinfo(int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(main_db, nkey, nextra);
  if (!k) return 0;
  for (int i = 0; i < nkey + nextra; i++) k->aColl[i] = main_db->pDfltColl;
  for (int i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

/* The sqlite_schema text the decoders were written against, compared with what the file holds
 * (SQLite keeps the CREATE statement as typed). Any difference: do not use the B-tree path. */
static int bt_sql_is(const char *name, const char *want) {
  sqlite3_stmt *s;
  int ok = 0;
  if (sqlite3_prepare_v2(main_db, "SELECT sql FROM sqlite_schema WHERE name = ?1", -1, &s, 0)) return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) {
    const char *t = (const char *)sqlite3_column_text(s, 0);
    ok = t && strcmp(t, want) == 0;
  }
  sqlite3_finalize(s);
  return ok;
}

int td_bt_init(char *err, int n) {
  static const char *checks[][2] = {
    {"SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at"},
    {"SELECT name FROM pragma_table_info('users')", "id,username,created_at"},
    {"SELECT name FROM pragma_table_info('likes')", "user_id,post_id,created_at"},
    {"SELECT name FROM pragma_index_info('posts_created_at_id_idx')", "created_at,id"},
    {"SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id"},
    {"SELECT desc FROM pragma_index_xinfo('posts_created_at_id_idx') WHERE key", "1,1"},
    {"SELECT desc FROM pragma_index_xinfo('likes_post_id_idx') WHERE key", "0"},
    {"SELECT type FROM pragma_table_info('posts')", "INTEGER,INTEGER,TEXT,TEXT"},
    {"SELECT type FROM pragma_table_info('users')", "INTEGER,TEXT,TEXT"},
    {"SELECT pk FROM pragma_table_info('posts')", "1,0,0,0"},
    {"SELECT pk FROM pragma_table_info('users')", "1,0,0"},
  };
  if (!main_db) { snprintf(err, n, "no database"); return -1; }
  for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
    if (!bt_layout(checks[i][0], checks[i][1])) {
      snprintf(err, n, "unexpected schema (%s)", checks[i][0]);
      return -1;
    }
  }
  if (!bt_sql_is("posts_created_at_id_idx", "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC)") ||
      !bt_sql_is("likes_post_id_idx", "CREATE INDEX likes_post_id_idx       ON likes (post_id)")) {
    snprintf(err, n, "unexpected index SQL");
    return -1;
  }
  if (ENC(main_db) != SQLITE_UTF8) { snprintf(err, n, "database text encoding is not UTF-8"); return -1; }
  root_posts = bt_rootpage("posts");
  root_users = bt_rootpage("users");
  root_feed_idx = bt_rootpage("posts_created_at_id_idx");
  root_likes_idx = bt_rootpage("likes_post_id_idx");
  if (!root_posts || !root_users || !root_feed_idx || !root_likes_idx) { snprintf(err, n, "root pages"); return -1; }
  ki_feed = bt_keyinfo(2, 1, 1);
  ki_likes = bt_keyinfo(1, 1, 0);
  if (!ki_feed || !ki_likes) { snprintf(err, n, "keyinfo"); return -1; }
  bt = main_db->aDb[0].pBt;
  for (int i = 0; i < NCUR; i++) {
    cur[i] = sqlite3_malloc(sqlite3BtreeCursorSize());
    if (!cur[i]) { snprintf(err, n, "cursor alloc"); return -1; }
  }
  return 0;
}

/* Called right after the BEGIN statement succeeded (BEGIN is deferred and takes no lock): joins the
 * read transaction at the B-tree level and opens the four cursors. 0 ok, -1 failed. */
int td_bt_open_cursors(void) {
  if (cur_open) return 0;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) return -1;
  static const Pgno *roots[NCUR] = {&root_posts, &root_users, &root_feed_idx, &root_likes_idx};
  KeyInfo *kis[NCUR] = {0, 0, ki_feed, ki_likes};
  for (int i = 0; i < NCUR; i++) {
    sqlite3BtreeCursorZero(cur[i]);
    if (sqlite3BtreeCursor(bt, *roots[i], 0, kis[i], cur[i]) != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3BtreeCloseCursor(cur[j]);
      return -1;
    }
  }
  cur_open = 1;
  return 0;
}

/* Before COMMIT or ROLLBACK. */
void td_bt_close_cursors(void) {
  if (!cur_open) return;
  for (int i = 0; i < NCUR; i++) sqlite3BtreeCloseCursor(cur[i]);
  cur_open = 0;
}

/* Is the batch's transaction still open at SQLite's level? (A failed statement can end it.) */
int td_in_txn(void) { return !sqlite3_get_autocommit(main_db); }

unsigned long td_cursor(int i) { return (unsigned long)cur[i]; }

/* Table cursor to rowid `id`: 1 found, 0 absent, -1 error. */
int td_seek(BtCursor *c, i64 id) {
  int res = 0;
  if (sqlite3BtreeTableMoveto(c, id, 0, &res) != SQLITE_OK) return -1;
  return res == 0 && c->eState == CURSOR_VALID;
}

/* Index cursor to the first entry whose leading integer column is >= k: 1 on an entry, 0 none. */
int td_seek_ge(BtCursor *c, i64 k) {
  Mem m;
  UnpackedRecord r;
  int res = 0;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = k;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = c->pKeyInfo;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1; /* entries equal on the key sort after it: land on the first of them */
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

/* Likes per post for lo <= post_id <= hi, read off likes_post_id_idx (post_id, rowid) in one walk:
 * n[post_id - lo] counts the entries (n has hi - lo + 1 zeroed slots). 0 ok, -1 error, 1 a cell not
 * in the expected form (the caller then decodes the entries itself). */
int td_count_likes(BtCursor *c, i64 lo, i64 hi, unsigned *n) {
  int r = td_seek_ge(c, lo);
  if (r <= 0) return r;
  for (;;) {
    MemPage *pg = c->pPage;
    int i = c->ix, last = pg->leaf ? pg->nCell : i + 1; /* an interior entry: just this one */
    for (; i < last; i++) {
      u8 *cell = findCell(pg, i) + pg->childPtrSize;
      u32 len = cell[0];
      const u8 *p = cell + 1;
      if (len >= 0x80 || len < 3 || p + len > pg->aDataEnd) return 1;
      u32 h = p[0], t = p[1];
      if (h >= len || h < 3 || t >= 0x80) return 1;
      i64 v;
      if (t >= 1 && t <= 6) {
        static const u8 sz[7] = {0, 1, 2, 3, 4, 6, 8};
        u32 k = sz[t];
        if (h + k > len) return 1;
        const u8 *q = p + h;
        v = (i8)q[0];
        for (u32 j = 1; j < k; j++) v = v << 8 | q[j];
      } else if (t == 8 || t == 9) {
        v = t - 8;
      } else {
        return 1;
      }
      if (v > hi) return 0;
      if (v >= lo) n[v - lo]++;
    }
    c->ix = (u16)(last - 1);
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
}

/* 1 on a row, 0 empty, -1 error. */
int td_first(BtCursor *c) {
  int empty = 1;
  if (sqlite3BtreeFirst(c, &empty) != SQLITE_OK) return -1;
  return !empty;
}

/* 1 on the next row, 0 at the end, -1 error. */
int td_next(BtCursor *c) {
  int rc = sqlite3BtreeNext(c, 0);
  if (rc == SQLITE_OK) return 1;
  return rc == SQLITE_DONE ? 0 : -1;
}

/* The current row's record bytes, valid until the cursor moves; *len gets the size. */
unsigned long td_payload(BtCursor *c, u32 *len) {
  getCellInfo(c);
  u32 total = c->info.nPayload;
  if (c->info.nLocal == total) { *len = total; return (unsigned long)c->info.pPayload; }
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 4096;
    spill = sqlite3_malloc(spill_cap);
    if (!spill) { spill_cap = 0; return 0; }
  }
  if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return 0;
  *len = total;
  return (unsigned long)spill;
}

/* ---- the helper thread ---------------------------------------------------------------------- */

/* Shared words. The event loop writes slots 0-8, 10 and 12-13 (plain stores); this thread writes 9
 * and 11; td_tick (loop thread) writes 14-19.
 *   0 turn_max_us (since the last report)   1 wait_max_us (since the last report)
 *   2 answer cap now                        3 open connections
 *   4 requests answered                     5 GCs so far
 *   6 passes                                7 turn_max_us overall
 *   8 wait_max_us overall                   9 MemAvailable kB (helper)
 *  10 requests answered in the last turn    11 checkpoints run by the helper
 *  12 write pulses (commits)                13 writes that waited for a pulse
 *  14 write holds                           15 holds that timed out
 *  16 longest hold, ms                      17 checkpoints the loop had to run itself (last resort)
 *  18 frames in the WAL now                 19 times the log was fully copied (it restarts next)
 *  20 requests answered from the kernel's receive buffer (no copy)
 *  21 pulses postponed because reads were backed up (PULSE_MAX_MS)
 *  22 passes that answered newest first (QUEUE=newest) */
volatile uint64_t td_stats[32];
static volatile int policy_applied;   /* 2 nice 19, 0 none */
static char ckpt_path[1024];
static int stats_on;

unsigned long td_stats_addr(void) { return (unsigned long)td_stats; }
int td_helper_policy(void) { return policy_applied; }

static long mem_available_kb(void) {
  char buf[2048];
  int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  char *p = strstr(buf, "MemAvailable:");
  return p ? atol(p + 13) : -1;
}

/* nice 19, not SCHED_IDLE: an idle-class thread gets nothing at all while the loop is busy, so a
 * checkpoint could never finish under load and the log would grow to the last-resort limit. */
static void apply_policy(void) {
  pid_t tid = (pid_t)syscall(SYS_gettid);
  if (setpriority(PRIO_PROCESS, tid, 19) == 0 && getpriority(PRIO_PROCESS, tid) == 19) {
    policy_applied = 2;
  } else {
    policy_applied = 0;
  }
  fprintf(stderr, "helper thread: %s\n", policy_applied == 2 ? "nice 19" : "default priority (nice refused)");
}

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

static void ck_set(int st) {
  pthread_mutex_lock(&ck_mu);
  __atomic_store_n(&ck_state, st, __ATOMIC_RELEASE);
  pthread_cond_signal(&ck_cv);
  pthread_mutex_unlock(&ck_mu);
}

static void *helper_main(void *arg) {
  (void)arg;
  apply_policy();
  sqlite3 *db = 0;
  /* Same VFS as the event loop's connection: in one process they share the exclusive lock and the
   * heap WAL index. */
  if (sqlite3_open_v2(ckpt_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, db_vfs) != SQLITE_OK) {
    fprintf(stderr, "helper thread: cannot open the database for checkpoints: %s\n",
            db ? sqlite3_errmsg(db) : "out of memory");
    db = 0;
  } else {
    sqlite3_busy_timeout(db, 0);
    /* A connection that has never read the database has not opened the WAL, and
     * sqlite3_wal_checkpoint_v2 on it does nothing (it returns OK with -1 frames). Read the schema
     * once so every checkpoint below is real. A checkpoint copies frames straight from the WAL, so
     * this connection needs almost no page cache. */
    char *msg = 0;
    if (sqlite3_exec(db, "PRAGMA wal_autocheckpoint = 0; PRAGMA cache_size = 16; SELECT count(*) FROM sqlite_schema",
                     0, 0, &msg) != SQLITE_OK) {
      fprintf(stderr, "helper thread: cannot read the database: %s\n", msg ? msg : "error");
      sqlite3_free(msg);
      sqlite3_close(db);
      db = 0;
    }
  }
  uint64_t last_report = mono_ms(), last_mem = 0;
  for (;;) {
    pthread_mutex_lock(&ck_mu);
    int st = ck_state;
    if (st != CK_BULK && st != CK_TAIL) {
      struct timespec dl;
      clock_gettime(CLOCK_MONOTONIC, &dl);
      dl.tv_nsec += 200 * 1000000L;
      if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&ck_cv, &ck_mu, &dl);
      st = ck_state;
    }
    pthread_mutex_unlock(&ck_mu);
    if (st == CK_BULK || st == CK_TAIL) {
      int nlog = -1, ndone = -1, rc = SQLITE_ERROR;
      if (ckpt_delay_ms > 0) {
        struct timespec d = {ckpt_delay_ms / 1000, (ckpt_delay_ms % 1000) * 1000000L};
        nanosleep(&d, 0);
      }
      if (db) rc = sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone);
      if (rc == SQLITE_OK && nlog >= 0) td_stats[11]++;
      pthread_mutex_lock(&ck_mu);
      ck_full = (rc == SQLITE_OK && nlog >= 0 && nlog == ndone);
      __atomic_store_n(&ck_state, st == CK_BULK ? CK_BULK_DONE : CK_TAIL_DONE, __ATOMIC_RELEASE);
      pthread_mutex_unlock(&ck_mu);
    }
    uint64_t now = mono_ms();
    if (now - last_mem >= 200) {
      last_mem = now;
      long kb = mem_available_kb();
      if (kb >= 0) td_stats[9] = (uint64_t)kb;
    }
    if (stats_on && now - last_report >= 5000) {
      last_report = now;
      uint64_t tm = td_stats[0], wm = td_stats[1];
      td_stats[0] = 0; td_stats[1] = 0;
      fprintf(stderr,
              "stats: reqs=%llu conns=%llu passes=%llu turn_max_us=%llu (overall %llu) "
              "wait_max_us=%llu (overall %llu) cap=%llu gcs=%llu mem_avail_kb=%llu ckpts=%llu "
              "pulses=%llu held_writes=%llu holds=%llu hold_timeouts=%llu hold_max_ms=%llu "
              "loop_ckpts=%llu wal_frames=%llu log_restarts=%llu inline=%llu stretched=%llu newest_first_passes=%llu\n",
              (unsigned long long)td_stats[4], (unsigned long long)td_stats[3],
              (unsigned long long)td_stats[6], (unsigned long long)tm,
              (unsigned long long)td_stats[7], (unsigned long long)wm,
              (unsigned long long)td_stats[8], (unsigned long long)td_stats[2],
              (unsigned long long)td_stats[5], (unsigned long long)td_stats[9],
              (unsigned long long)td_stats[11], (unsigned long long)td_stats[12],
              (unsigned long long)td_stats[13], (unsigned long long)td_stats[14],
              (unsigned long long)td_stats[15], (unsigned long long)td_stats[16],
              (unsigned long long)td_stats[17], (unsigned long long)td_stats[18],
              (unsigned long long)td_stats[19], (unsigned long long)td_stats[20],
              (unsigned long long)td_stats[21], (unsigned long long)td_stats[22]);
    }
  }
  return 0;
}

/* The event loop calls this once per pass, after the pass's commit (so never inside a write
 * transaction). It drives the checkpoint thread and returns what the loop has to do:
 *   bit 0  writes are held: do not run the write pulse (reads go on);
 *   bit 1  the log is fully copied: end the read transaction now, so the next one starts on the
 *          copied log and the next write restarts the log;
 *   bit 2  last resort: end the read transaction and call td_force_ckpt(). */
int td_tick(void) {
  int r = 0;
  switch (__atomic_load_n(&ck_state, __ATOMIC_ACQUIRE)) {
  case CK_IDLE:
    if (wal_frames >= ckpt_force) r |= 4;
    else if (wal_frames >= ckpt_frames) { ck_seq0 = commit_seq; ck_set(CK_BULK); }
    break;
  case CK_BULK_DONE:
    /* SQLite reports "everything copied" against the log as it was when the checkpoint began, so
     * the answer only stands if the loop has not committed since. */
    if (ck_full && commit_seq == ck_seq0) {   /* nothing was written meanwhile: no hold needed */
      wal_frames = 0;
      td_stats[19]++;
      r |= 2;
      ck_set(CK_IDLE);
    } else {
      writes_held = 1;
      td_stats[14]++;
      clock_gettime(CLOCK_MONOTONIC, &hold_t0);
      ck_seq0 = commit_seq;
      ck_set(CK_TAIL);
    }
    break;
  case CK_TAIL_DONE: {
    /* Fully copied: the next write restarts the log and the WAL hook reports its new size.
     * Otherwise (the hold timed out and writes went on) the next tick starts another round. */
    if (ck_full && commit_seq == ck_seq0) { wal_frames = 0; td_stats[19]++; r |= 2; }
    if (writes_held) {
      uint64_t ms = (uint64_t)ms_since(&hold_t0);
      if (ms > td_stats[16]) td_stats[16] = ms;
    }
    writes_held = 0;
    ck_set(CK_IDLE);
    break;
  }
  default:                              /* a checkpoint is running */
    if (writes_held && ms_since(&hold_t0) > HOLD_MAX_MS) {
      writes_held = 0;
      td_stats[15]++;
      td_stats[16] = HOLD_MAX_MS;
    }
    break;
  }
  td_stats[18] = (uint64_t)wal_frames;
  return r | (writes_held ? 1 : 0);
}

/* Last resort (see td_tick): the loop copies the log back itself. Called with no transaction open
 * and the thread idle, so nothing else reads and the checkpoint is complete. */
int td_force_ckpt(void) {
  int nlog = 0, ndone = 0;
  td_stats[17]++;
  if (sqlite3_wal_checkpoint_v2(main_db, 0, SQLITE_CHECKPOINT_PASSIVE, &nlog, &ndone) == SQLITE_OK && nlog == ndone) {
    wal_frames = 0;
    td_stats[19]++;
    return 0;
  }
  return -1;
}

int td_start_helper(const char *path, int stats) {
  pthread_t t;
  pthread_attr_t at;
  pthread_condattr_t ca;
  snprintf(ckpt_path, sizeof ckpt_path, "%s", path);
  stats_on = stats;
  /* testing aids: frames in the log that start a checkpoint / that make the loop checkpoint itself,
   * and a delay before each checkpoint of the thread */
  const char *e = getenv("CKPT_FRAMES");
  if (e && atoi(e) >= 16) ckpt_frames = atoi(e);
  e = getenv("CKPT_FORCE");
  if (e && atoi(e) > ckpt_frames) ckpt_force = atoi(e);
  e = getenv("CKPT_DELAY_MS");
  if (e && atoi(e) > 0) ckpt_delay_ms = atoi(e);
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&ck_cv, &ca);
  pthread_condattr_destroy(&ca);
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 1024 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&t, &at, helper_main, 0);
  pthread_attr_destroy(&at);
  return rc;
}
