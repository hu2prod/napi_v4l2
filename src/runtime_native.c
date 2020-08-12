// memset
#include<string.h>
#include<stdlib.h>
#include "runtime.h"
////////////////////////////////////////////////////////////////////////////////////////////////////
// special env
////////////////////////////////////////////////////////////////////////////////////////////////////
__attribute__((visibility("default"))) u32 HISTORY_DEPTH = 1;
__attribute__((visibility("default"))) u32 SIZE_X = 1;
__attribute__((visibility("default"))) u32 SIZE_Y = 1;
__attribute__((visibility("default"))) u32 SIZE_XY = 1;
__attribute__((visibility("default"))) u32 SIZE_XY_PX = 3;
////////////////////////////////////////////////////////////////////////////////////////////////////
// bump allocator
////////////////////////////////////////////////////////////////////////////////////////////////////
// TODO get __heap_base from global
// extern unsigned char __heap_base;
// unsigned int bump_pointer = &__heap_base;

size_t bump_pointer = 0;
size_t __heap_start = 0; // т.к. heap_base не доступна из под clang

// TODO remove
__attribute__((visibility("default")))
void __heap_set(size_t _bump_pointer) {
  bump_pointer = _bump_pointer;
  __heap_start = _bump_pointer;
}

u32 total_alloc_size = 0;
u32 total_alloc_size_page = 0;
u32 page_size = 16*1024;
__attribute__((visibility("default")))
extern void* ws_malloc(u32 size, char* tag) {
  void *res = malloc(size);
  if (!res) {
    user_throw("malloc fail");
  }
  memset(res, 0, size);
  return res;
  // // putsi("ws_malloc  ", size);
  // // putssi("ws_malloc", tag, size);
  // total_alloc_size += size;
  // u32 new_page_size = total_alloc_size/page_size;
  // u32 page_diff = new_page_size - total_alloc_size_page;
  // total_alloc_size_page = new_page_size;
  // 
  // if (page_diff) {
  //   putsi("grow", page_diff);
  //   __builtin_wasm_memory_grow(0, page_diff);
  // }
  // unsigned int r = bump_pointer;
  // bump_pointer += ((size / MEMORY_ALIGN) + 1) * MEMORY_ALIGN;
  // return (void *)r;
}

void ws_free(void* ptr) {
  free(ptr);
  // NOPE
  // putsi("free giveup memory leak", (i32)ptr);
}

u32 header_offset = 4*16;

u32 __alloc_log2_bucket_size  = 32;
u32 __alloc_log2_bucket_count = 64; // привет 64 бита
void*** __array_log2_list;
__attribute__((visibility("default")))
void __alloc_init() {
  // mini  inject
  u32 size  = __alloc_log2_bucket_count;
  // 2* for 64-bit
  size += header_offset; // mimic array (-3 length)
  __array_log2_list = (void***)ws_malloc(8*size, "__alloc_init");
  for(u32 i=0;i<__alloc_log2_bucket_count;i++) {
    // -1 REF_COUNT       override -> start_pointer
    // -2 RTTI            override -> end_pointer
    // -3 LEN             override (NOT USED)
    // -4 CAP (RESERVED)  override (NOT USED)
    __array_log2_list[i] = (void**)(ws_malloc(sizeof(size_t)*__alloc_log2_bucket_size + header_offset, "__alloc_init") + header_offset);
  }
}
////////////////////////////////////////////////////////////////////////////////////////////////////

__attribute__((visibility("default"))) const i32 CAPACITY_OFFSET = -4;
__attribute__((visibility("default"))) const i32 LENGTH_OFFSET   = -3;
__attribute__((visibility("default"))) const i32 RTTI_OFFSET     = -2;
__attribute__((visibility("default"))) const i32 REF_COUNT_OFFSET= -1;

void* __alloc_ref(u32 size) {
  // rtti
  // ref_count
  size += 8;
  size_t r = (size_t)ws_malloc(size, "__alloc_ref");
  r += 8;
  return (void *)r;
}

// неправильный тип
// void** __alloc_array(u32 size, const char* tag) {
void* __alloc_array(u32 size, const char* tag) {
  // super simple pool
  if (__builtin_popcount(size) != 1) {
    // putsi("suboptimal alloc size", size);
  } else {
    u32 log2 = __builtin_clzl(size);
    size_t** bucket = (size_t**)__array_log2_list[log2];
    size_t* bucket_view_size_t= (size_t*)bucket;
    size_t bucket_start  = bucket_view_size_t[-1];
    size_t bucket_end    = bucket_view_size_t[-2];
    
    // if (bucket_end == bucket_start) {
      // putsi("bucket_end == bucket_start size", size);
    // }
    if (bucket_start != bucket_end) {
      bucket_view_size_t[-1] = (bucket_start + 1) % __alloc_log2_bucket_count;
      return (void**)bucket_view_size_t[bucket_start];
    }
  }
  
  // logssi("__alloc_array", tag, size);
  // capacity
  // length
  // rtti
  // ref_count
  size += header_offset;
  size_t r = (size_t)ws_malloc(size, "__alloc_array");
  r += header_offset;
  return (void *)r;
}

// сильно много warning'ов на типы
// void __free_array(void** ptr) {
void __free_array(void* ptr) {
  size_t* ptr_view_size_t = (size_t*)ptr;
  size_t capacity = ptr_view_size_t[CAPACITY_OFFSET];
  if (__builtin_popcount(capacity) != 1) {
    // putsi("fragmentation of bad alloc size", capacity);
    ws_free((void**)((size_t)ptr - header_offset));
  } else {
    u32 log2 = __builtin_clzl(capacity);
    size_t** bucket = (size_t**)__array_log2_list[log2];
    size_t* bucket_view_size_t= (size_t*)bucket;
    size_t bucket_start  = bucket_view_size_t[-1];
    size_t bucket_end    = bucket_view_size_t[-2];
    size_t bucket_new_end = (bucket_end+1) % __alloc_log2_bucket_count;
    if (bucket_start == bucket_new_end) {
      ws_free((void**)((size_t)ptr - header_offset)); // give up
      return;
    }
    bucket_view_size_t[bucket_end] = (size_t)ptr;
    
    // bucket_end = bucket_new_end;
    bucket_view_size_t[-2] = bucket_new_end;
  }
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// outside helper
////////////////////////////////////////////////////////////////////////////////////////////////////

u32 debug_counter = 0;

__attribute__((visibility("default")))
u32 u32_get(u32* t) {
  return *t;
}

__attribute__((visibility("default")))
void u32_set(u32* t, u32 val) {
  *t = val;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// outside helper
////////////////////////////////////////////////////////////////////////////////////////////////////
// for temp
void logi( i32 n )                                {printf("%d\n", n);};
void logsi( const char* str, i32 n )              {printf("%s %d\n", str, n);};
void logssi( const char* str, char* str2, i32 n ) {printf("%s %s %d\n", str, str2, n);};
void logsf( const char* str, f32 n )              {printf("%s %f\n", str, n);};

// for persistant
void puti( i32 n )                                {printf("%d\n", n);};
void putsi( const char* str, i32 n )              {printf("%s %d\n", str, n);};
void putssi( const char* str, char* str2, i32 n ) {printf("%s %s %d\n", str, str2, n);};
void putsf( const char* str, f32 n )              {printf("%s %f\n", str, n);};

void user_throw( const char* str ) {printf("THROW %s\n", str); exit(-1);}


////////////////////////////////////////////////////////////////////////////////////////////////////
// extra util. TODO move somewhere...
////////////////////////////////////////////////////////////////////////////////////////////////////
#include "array8.c"
#include "array_size_t.c"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

char* file_read(const char* path, size_t *ref_size) {
  struct stat buf;
  stat(path, &buf);
  size_t size = buf.st_size;
  
  FILE *fp;
  if ((fp=fopen(path, "rb"))==NULL) {
    printf("Cannot open file %s.\n", path);
    exit(1);
  }
  char* cont = array8_alloc(size+1);
  array8_length_set_unsafe(cont, size+1);
  size_t real_size = fread(cont, 1, size, fp);
  if (real_size != size) {
    // printf("cont      = '%s'\n", cont);
    printf("path      = %s\n", path);
    printf("real_size = %ld\n", real_size);
    printf("     size = %ld\n",      size);
    printf("ferror = %d\n", ferror(fp));
    printf("feof   = %d\n", feof(fp));
    user_throw("real_size != size");
  }
  cont[size] = '\0';
  fclose(fp);
  
  if (ref_size) {
    *ref_size = size;
  }
  
  return cont;
}

bool file_exists(const char* path) {
  return access(path, F_OK) != -1;
}

// getenv doesn't need free
// file_read needs array8_free()
char* conf_read(const char* key) {
  char* value = getenv(key);
  if (value) {
    size_t len = strlen(value);
    char *ret = array8_alloc(len+1);
    ret[len] = 0;
    strcpy(ret, value);
    return ret;
  }
  
  return file_read(key, NULL);
}

int u64_cmp(const void * va, const void * vb) {
  u64 a = *((u64*)va);
  u64 b = *((u64*)vb);
  return a - b;
}

void debug_checkpoint(const char* key) {
  const char* path = "debug_checkpoint";
  FILE *fp;
  if ((fp=fopen(path, "wb"))==NULL) {
    printf("Cannot open file %s.\n", path);
    exit(1);
  }
  fwrite(key, strlen(key), 1, fp);
  fclose(fp);
}

#define dc \
  printf("dc %s:%d\n", __FILE__, __LINE__);

void* __global_tmp_malloc;
// special fast debug
/*
// #define malloc(t) \
//   ( \
//   printf("try malloc %s:%d\n", __FILE__, __LINE__), \
//   __global_tmp_malloc = malloc(t), \
//   printf("try malloc %s:%d OK %lx\n", __FILE__, __LINE__, (unsigned long)__global_tmp_malloc), \
//   __global_tmp_malloc \
//   )
// 
// #define free(t) \
//   printf("try free %s:%d %lx\n", __FILE__, __LINE__, (unsigned long)(t)); \
//   free(t); \
//   printf("try free %s:%d OK\n", __FILE__, __LINE__);
// 
// // special fast debug2
// #define ws_malloc(t, tag) \
//   ( \
//   printf("try ws_malloc %s:%d\n", __FILE__, __LINE__), \
//   __global_tmp_malloc = ws_malloc(t, tag), \
//   printf("try ws_malloc %s:%d OK %lx\n", __FILE__, __LINE__, (unsigned long)__global_tmp_malloc), \
//   __global_tmp_malloc \
//   )
// 
// #define ws_free(t) \
//   printf("try ws_free %s:%d %lx\n", __FILE__, __LINE__, (unsigned long)(t)); \
//   ws_free(t); \
//   printf("try ws_free %s:%d OK\n", __FILE__, __LINE__);

// #define free(t) \
  // printf("try free %s:%d %lx\n", __FILE__, __LINE__, (unsigned long)(t));printf("try free %s:%d OK\n", __FILE__, __LINE__);

*/