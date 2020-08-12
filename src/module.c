#include <node_api.h>

#include "capture.c"
#include "runtime_native.c"
#include "array_size_t.c"
#include "hash.c"

u32 free_context_counter = 0;
void* free_context_fifo = NULL;
void** alloc_context_hash = NULL;

void free_capture_context_fifo_ensure_init() {
  if (free_context_fifo == NULL) {
    free_context_fifo = array_size_t_alloc(16);
    alloc_context_hash = hash_size_t_alloc();
  }
}

// non thread-safe
struct Capture_context* capture_context_by_id(int id) {
  return (struct Capture_context*)hash_size_t_get(alloc_context_hash, id);
}



napi_value start(napi_env env, napi_callback_info info) {
  napi_status status;
  
  napi_value ret_dummy;
  status = napi_create_int32(env, 0, &ret_dummy);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_dummy");
    return ret_dummy;
  }
  
  size_t argc = 6;
  napi_value argv[6];
  status = napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Failed to parse arguments");
    return ret_dummy;
  }
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  size_t video_dev_path_len;
  status = napi_get_value_string_utf8(env, argv[0], NULL, 0, &video_dev_path_len);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Can't get video_dev_path_len");
    return ret_dummy;
  }
  
  size_t tmp;
  char *video_dev_path = malloc(video_dev_path_len);
  status = napi_get_value_string_utf8(env, argv[0], video_dev_path, video_dev_path_len+1, &tmp);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Can't get video_dev_path");
    return ret_dummy;
  }
  
  i32 size_x;
  status = napi_get_value_int32(env, argv[1], &size_x);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid i32 was passed as argument of size_x");
    return ret_dummy;
  }
  
  i32 size_y;
  status = napi_get_value_int32(env, argv[2], &size_y);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid i32 was passed as argument of size_y");
    return ret_dummy;
  }
  
  i32 fps;
  status = napi_get_value_int32(env, argv[3], &fps);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid i32 was passed as argument of fps");
    return ret_dummy;
  }
  
  i32 fps_div;
  status = napi_get_value_int32(env, argv[4], &fps_div);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid i32 was passed as argument of fps_div");
    return ret_dummy;
  }
  
  // for future 
  //  0 -> BGR24
  //  1 -> RGB24
  // see capture.c _capture_init_device
  
  i32 px_format;
  status = napi_get_value_int32(env, argv[5], &px_format);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid i32 was passed as argument of px_format");
    return ret_dummy;
  }
  
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  struct Capture_context* ctx;
  int ctx_idx;
  if (array_size_t_length_get(free_context_fifo)) {
    ctx = (struct Capture_context*)array_size_t_pop(free_context_fifo);
    ctx_idx = ctx->ctx_idx;
  } else {
    // alloc
    ctx = (struct Capture_context*)malloc(sizeof(struct Capture_context));
    ctx_idx = free_context_counter++;
    ctx->ctx_idx = ctx_idx;
    alloc_context_hash = hash_size_t_set(alloc_context_hash, ctx_idx, (size_t)ctx);
  }
  
  ctx->size_x = size_x;
  ctx->size_y = size_y;
  ctx->fps    = fps;
  ctx->fps_div= fps_div;
  ctx->buffer = (u8*)malloc(3*size_x*size_y); // BGR24 because hardcoded settings
  ctx->frame_id = 0; // NOTE u64
  ctx->px_format = px_format;
  ctx->capture_in_progress = false;
  
  ctx->live           = false;
  ctx->need_shutdown  = false;
  ctx->video_dev_path = video_dev_path;
  ctx->last_error     = NULL;
  ctx->last_error_need_free = false;
  
  ctx->_thread_id = (pthread_t)NULL;
  ctx->_fd        = 0;
  ctx->_io_type   = IO_METHOD_MMAP;
  ctx->_buffers   = NULL;
  ctx->_n_buffers = 0;
  
  ERR_RET_TYPE ret = capture_start(ctx);
  if (!ret) {
    // TODO pass better string
    napi_throw_error(env, NULL, "Unhandled napi_v4l2 error");
    return ret_dummy;
  }
  
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  napi_value ret_idx;
  status = napi_create_int32(env, ctx->ctx_idx, &ret_idx);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_idx");
    return ret_dummy;
  }
  
  return ret_idx;
}


napi_value stop(napi_env env, napi_callback_info info) {
  napi_status status;
  
  napi_value ret_dummy;
  status = napi_create_int32(env, 0, &ret_dummy);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_dummy");
    return ret_dummy;
  }
  
  size_t argc = 6;
  napi_value argv[6];
  status = napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Failed to parse arguments");
    return ret_dummy;
  }
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  i32 ctx_idx;
  status = napi_get_value_int32(env, argv[0], &ctx_idx);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid i32 was passed as argument of ctx_idx");
    return ret_dummy;
  }
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  //    checks
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  
  struct Capture_context* ctx = capture_context_by_id(ctx_idx);
  if (!ctx) {
    napi_throw_error(env, NULL, "Invalid ctx_idx");
    return ret_dummy;
  }
  
  if (!ctx->live) {
    napi_throw_error(env, NULL, "!ctx->live");
    return ret_dummy;
  }
  
  if (ctx->need_shutdown) {
    napi_throw_error(env, NULL, "ctx->need_shutdown");
    return ret_dummy;
  }
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  capture_stop(ctx);
  free_context_fifo = array_size_t_push(free_context_fifo, (size_t)ctx);
  
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  return ret_dummy;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
napi_value frame_get(napi_env env, napi_callback_info info) {
  napi_status status;
  
  napi_value ret_dummy;
  status = napi_create_int32(env, 0, &ret_dummy);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_dummy");
    return ret_dummy;
  }
  
  size_t argc = 3;
  napi_value argv[3];
  status = napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Failed to parse arguments");
    return ret_dummy;
  }
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  i32 ctx_idx;
  status = napi_get_value_int32(env, argv[0], &ctx_idx);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid i32 was passed as argument of ctx_idx");
    return ret_dummy;
  }
  
  // TODO buffer
  u8 *data_dst;
  size_t data_dst_len;
  status = napi_get_buffer_info(env, argv[1], (void**)&data_dst, &data_dst_len);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid buffer was passed as argument of data_dst");
    return ret_dummy;
  }
  
  i32 last_frame_id;
  status = napi_get_value_int32(env, argv[2], &last_frame_id);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Invalid i32 was passed as argument of last_frame_id");
    return ret_dummy;
  }
  
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  struct Capture_context* ctx = capture_context_by_id(ctx_idx);
  if (!ctx) {
    napi_throw_error(env, NULL, "Invalid ctx_idx");
    return ret_dummy;
  }
  
  size_t last_frame_size = 0;
  i32 frame_id = 0;
  bool capture_ok = false;
  
  for(i32 retry_id = 0; retry_id < 10; retry_id++) {
    // wait 100 mcs*10 = 1 ms (or more)
    for(i32 wait_id = 0; wait_id < 10; wait_id++) {
      if (ctx->capture_in_progress) {
        // usleep(100); // obsolete
        struct timespec tw = {0, 100000};
        struct timespec tr; // remaining time if delay was shorter
        nanosleep (&tw, &tr);
        continue;
      }
      break;
    }
    if (ctx->capture_in_progress) {
      continue;
    }
    
    last_frame_size = ctx->frame_size;
    if (data_dst_len < last_frame_size) {
      napi_throw_error(env, NULL, "data_dst_len < 3*ctx->size_x*ctx->size_y. Your buffer is too small");
      return ret_dummy;
    }
    
    // only in range of positive int32
    frame_id = ctx->frame_id & 0x1FFFFFFF;
    if (frame_id != last_frame_id) {
      memcpy(data_dst, (u8*)ctx->buffer, last_frame_size);
    }
    if (ctx->capture_in_progress) {
      // we copied possibly corrupted data, throw out and retry
      continue;
    }
    capture_ok = true;
    break;
  }
  
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  
  napi_value ret_arr;
  status = napi_create_array(env, &ret_arr);
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value");
    return ret_dummy;
  }
  
  // frame_id
  napi_value ret_frame_id;
  status = napi_create_int32(env, frame_id, &ret_frame_id);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_frame_id");
    return ret_dummy;
  }
  status = napi_set_element(env, ret_arr, 0, ret_frame_id);
  
  // frame_size
  napi_value ret_frame_size;
  status = napi_create_int32(env, last_frame_size, &ret_frame_size);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_frame_size");
    return ret_dummy;
  }
  status = napi_set_element(env, ret_arr, 1, ret_frame_size);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_frame_size assign");
    return ret_dummy;
  }
  // capture_ok
  i32 int_capture_ok = capture_ok?1:0;
  napi_value ret_capture_ok;
  status = napi_create_int32(env, int_capture_ok, &ret_capture_ok);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_capture_ok");
    return ret_dummy;
  }
  status = napi_set_element(env, ret_arr, 2, ret_capture_ok);
  
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to create return value ret_capture_ok assign");
    return ret_dummy;
  }
  
  
  
  return ret_arr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

napi_value Init(napi_env env, napi_value exports) {
  napi_status status;
  napi_value fn;
  
  __alloc_init();
  free_capture_context_fifo_ensure_init();
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  status = napi_create_function(env, NULL, 0, start, NULL, &fn);
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to wrap native function");
  }
  
  status = napi_set_named_property(env, exports, "start", fn);
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to populate exports");
  }
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  status = napi_create_function(env, NULL, 0, stop, NULL, &fn);
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to wrap native function");
  }
  
  status = napi_set_named_property(env, exports, "stop", fn);
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to populate exports");
  }
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  status = napi_create_function(env, NULL, 0, frame_get, NULL, &fn);
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to wrap native function");
  }
  
  status = napi_set_named_property(env, exports, "frame_get", fn);
  if (status != napi_ok) {
    napi_throw_error(env, NULL, "Unable to populate exports");
  }
  ////////////////////////////////////////////////////////////////////////////////////////////////////
  
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)