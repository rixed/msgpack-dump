#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

struct ctx {
  int fd;
  size_t offset;
  unsigned indent;
  bool eof;
};

static void ctx_ctor(struct ctx *ctx, int fd)
{
  ctx->fd = fd;
  ctx->offset = 0;
  ctx->indent = 0;
  ctx->eof = false;
}

#define ROLE_NONE -1
#define ROLE_MAP_KEY -2
#define ROLE_MAP_VALUE -3
// >=0 roles are array indexes

static bool dump(struct ctx *, int role);

static void dump_indent(struct ctx *ctx)
{
  // TODO: faster version
# define TAB 3
  for (unsigned t = 0; t < ctx->indent*TAB; t++) printf(" ");
# undef TAB
}

static void dump_start(struct ctx *ctx, int role)
{
  if (role != ROLE_MAP_VALUE) {
    dump_indent(ctx);
  }
  if (role >= 0) {
    printf("[%d]: ", role);
  }
}

static void dump_stop(struct ctx *ctx, int role)
{
  (void)ctx;
  if (role == ROLE_MAP_KEY) {
    printf(": ");
  } else {
    printf("\n");
  }
}

// Error checked IO
static bool eread(struct ctx *ctx, void *buf_, size_t sz)
{
  unsigned char *buf = buf_;
  if (ctx->eof) return false;

  size_t done = 0;
  while (done < sz) {
    ssize_t ret = read(ctx->fd, buf+done, sz-done);
    if (ret == 0) {
      ctx->eof = true;
      return false;
    } else if (ret < 0) {
      fprintf(stderr, "Cannot read %zu bytes: %s\n", sz, strerror(errno));
      return false;
    }
    done += ret;
    ctx->offset += ret;
  }
  return true;
}


static void dump_nil(struct ctx *ctx)
{
  (void)ctx;
  printf("()");
}

static void dump_false(struct ctx *ctx)
{
  (void)ctx;
  printf("false");
}

static void dump_true(struct ctx *ctx)
{
  (void)ctx;
  printf("true");
}

static void dump_int(struct ctx *ctx, int n)
{
  (void)ctx;
  printf("%d", n);
}

static bool read_varint(struct ctx *ctx, uint64_t *n, size_t lenlen, bool sign)
{
  *n = 0;
  unsigned char byte;
  for (unsigned cb = 0; cb < lenlen; cb++) {
    *n <<= 8;  // big endian is not dead
    if (! eread(ctx, &byte, 1)) return false;
    *n |= byte;
  }
  if (sign) {
    uint64_t sign_extend = ~0ULL << lenlen;
    if (byte >= 128) *n |= sign_extend;
  }
  return true;
}
static bool read_varuint(struct ctx *ctx, uint64_t *n, size_t lenlen)
{
  return read_varint(ctx, n, lenlen, false);
}

static bool dump_varint(struct ctx *ctx, size_t lenlen, bool sign)
{
  uint64_t n;
  if (! read_varint(ctx, &n, lenlen, sign)) return false;

  if (sign) {
    printf("%"PRId64, (int64_t)n);
  } else {
    printf("%"PRIu64, n);
  }
  return true;
}

static bool dump_varuint(struct ctx *ctx, size_t b) { return dump_varint(ctx, b, false); }
static bool dump_varsint(struct ctx *ctx, size_t b) { return dump_varint(ctx, b, true); }
static bool dump_uint8(struct ctx *ctx) { return dump_varuint(ctx, 1); }
static bool dump_uint16(struct ctx *ctx) { return dump_varuint(ctx, 2); }
static bool dump_uint32(struct ctx *ctx) { return dump_varuint(ctx, 4); }
static bool dump_uint64(struct ctx *ctx) { return dump_varuint(ctx, 8); }
static bool dump_int8(struct ctx *ctx) { return dump_varsint(ctx, 1); }
static bool dump_int16(struct ctx *ctx) { return dump_varsint(ctx, 2); }
static bool dump_int32(struct ctx *ctx) { return dump_varsint(ctx, 4); }
static bool dump_int64(struct ctx *ctx) { return dump_varsint(ctx, 8); }

static bool dump_float32(struct ctx *ctx)
{
  float v;
  assert(sizeof(v) == 4);
  if (! eread(ctx, &v, sizeof(v))) return false;
  printf("%g", v);
  return true;
}

static bool dump_float64(struct ctx *ctx)
{
  double v;
  assert(sizeof(v) == 8);
  if (! eread(ctx, &v, sizeof(v))) return false;
  printf("%g", v);
  return true;
}

static bool dump_data(struct ctx *ctx, bool is_str, size_t len)
{
  char *data = malloc(len);
  if (! data) {
    fprintf(stderr, "Cannot alloc %zu bytes", len);
    return false;
  }
  if (! eread(ctx, data, len)) {
    free(data);
    return false;
  }

  if (is_str) {
    printf("\"%.*s\"", (int)len, data);
  } else {
    for (unsigned n = 0; n < len; n++) {
      printf("%s%02x", n > 0 ? " ":"", data[n]);
    }
  }
  free(data);
  return true;
}

static bool dump_data_var(struct ctx *ctx, bool is_str, size_t lenlen)
{
  uint64_t len;
  if (! read_varuint(ctx, &len, lenlen)) return false;
  return dump_data(ctx, is_str, len);
}

static bool dump_array(struct ctx *ctx, size_t nb_objs)
{
  printf("[\n");
  ctx->indent ++;

  for (unsigned n = 0; n < nb_objs; n++) {
    if (! dump(ctx, n)) return false;
  }

  ctx->indent--;
  dump_indent(ctx);
  printf("]");
  return true;
}

static bool dump_array_var(struct ctx *ctx, size_t lenlen)
{
  uint64_t len;
  if (! read_varuint(ctx, &len, lenlen)) return false;
  return dump_array(ctx, len);
}

static bool dump_map(struct ctx *ctx, size_t nb_objs)
{
  printf("{\n");
  ctx->indent ++;

  for (unsigned n = 0; n < nb_objs; n++) {
    if (! dump(ctx, ROLE_MAP_KEY)) return false;
    if (! dump(ctx, ROLE_MAP_VALUE)) return false;
  }

  ctx->indent --;
  dump_indent(ctx);
  printf("}");
  return true;
}

static bool dump_map_var(struct ctx *ctx, size_t lenlen)
{
  uint64_t len;
  if (! read_varuint(ctx, &len, lenlen)) return false;
  return dump_map(ctx, len);
}

static bool dump_ext(struct ctx *ctx, size_t len)
{
  unsigned char type;
  if (! eread(ctx, &type, 1)) return false;
  printf("Type%d:", type);
  dump_data(ctx, false, len);
  return true;
}
static bool dump_ext_var(struct ctx *ctx, size_t lenlen)
{
  uint64_t len;
  if (! read_varuint(ctx, &len, lenlen)) return false;
  return dump_ext(ctx, len);
}

static bool dump(struct ctx *ctx, int role)
{
  unsigned char fst;
  if (! eread(ctx, &fst, 1)) return ctx->eof;

  dump_start(ctx, role);

  if (fst == 0xc0) dump_nil(ctx);
  else if (fst == 0xc2) dump_false(ctx);
  else if (fst == 0xc3) dump_true(ctx);
  else if ((fst & 0x80) == 0) dump_int(ctx, fst);
  else if ((fst & 0xe0) == 0xe0) dump_int(ctx, fst);
  else if (fst == 0xcc) {
    if (! dump_uint8(ctx)) return false;
  } else if (fst == 0xcd) {
    if (! dump_uint16(ctx)) return false;
  } else if (fst == 0xce) {
    if (! dump_uint32(ctx)) return false;
  } else if (fst == 0xcf) {
    if (! dump_uint64(ctx)) return false;
  } else if (fst == 0xd0) {
    if (! dump_int8(ctx)) return false;
  } else if (fst == 0xd1) {
    if (! dump_int16(ctx)) return false;
  } else if (fst == 0xd2) {
    if (! dump_int32(ctx)) return false;
  } else if (fst == 0xd3) {
    if (! dump_int64(ctx)) return false;
  } else if (fst == 0xca) {
    if (! dump_float32(ctx)) return false;
  } else if (fst == 0xcb) {
    if (! dump_float64(ctx)) return false;
  } else if ((fst & 0xe0) == 0xa0) {
    if (! dump_data(ctx, true, fst & 0x1f)) return false;
  } else if (fst == 0xd9) {
    if (! dump_data_var(ctx, true, 1)) return false;
  } else if (fst == 0xda) {
    if (! dump_data_var(ctx, true, 2)) return false;
  } else if (fst == 0xdb) {
    if (! dump_data_var(ctx, true, 4)) return false;
  } else if (fst == 0xc4) {
    if (! dump_data_var(ctx, false, 1)) return false;
  } else if (fst == 0xc5) {
    if (! dump_data_var(ctx, false, 2)) return false;
  } else if (fst == 0xc6) {
    if (! dump_data_var(ctx, false, 4)) return false;
  } else if ((fst & 0xf0) == 0x90) {
    if (! dump_array(ctx, fst & 0x0f)) return false;
  } else if (fst == 0xdc) {
    if (! dump_array_var(ctx, 2)) return false;
  } else if (fst == 0xdd) {
    if (! dump_array_var(ctx, 4)) return false;
  } else if ((fst & 0xf0) == 0x80) {
    if (! dump_map(ctx, fst & 0x0f)) return false;
  } else if (fst == 0xde) {
    if (! dump_map_var(ctx, 2)) return false;
  } else if (fst == 0xdf) {
    if (! dump_map_var(ctx, 4)) return false;
  } else if (fst == 0xd4) {
    if (! dump_ext(ctx, 1)) return false;
  } else if (fst == 0xd5) {
    if (! dump_ext(ctx, 2)) return false;
  } else if (fst == 0xd6) {
    if (! dump_ext(ctx, 4)) return false;
  } else if (fst == 0xd7) {
    if (! dump_ext(ctx, 8)) return false;
  } else if (fst == 0xd8) {
    if (! dump_ext(ctx, 16)) return false;
  } else if (fst == 0xc7) {
    if (! dump_ext_var(ctx, 1)) return false;
  } else if (fst == 0xc8) {
    if (! dump_ext_var(ctx, 2)) return false;
  } else if (fst == 0xc9) {
    if (! dump_ext_var(ctx, 4)) return false;
  } else {
    fprintf(stderr, "Bad tag %02x\n", fst);
    return false;
  }
  
  dump_stop(ctx, role);
  return true;
}

int main(int nb_args, char **args)
{
  char *fname;
  switch (nb_args) {
    case 1:
      fname = "/dev/stdin";
      break;
    case 2:
      fname = args[1];
      break;
    default:
      printf("%s [file]\n", args[0]);
      exit(1);
  }

  int fd = open(fname, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "Cannot open input file '%s': %s\n", fname, strerror(errno));
    exit(1);
  }

  struct ctx ctx;
  ctx_ctor(&ctx, fd);
  while (! ctx.eof) {
    if (! dump(&ctx, ROLE_NONE)) {
      exit(1);
    }
  }

  close(fd);
}
