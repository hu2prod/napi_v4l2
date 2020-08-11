// inpired by https://gist.github.com/maxlapshin/1253534
// TODO check that all includes are really needed

// TODO un-hardcode V4L2_PIX_FMT_BGR24
// TODO try V4L2_PIX_FMT_RGB24

#include <string.h>
#include <assert.h>
#include <time.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <malloc.h>

#include "type.h"

// TODO pass errors to nodejs

#define ERR_RET_TYPE bool
#define ERR_RET_ERROR \
  return false;

// TODO implement pass str to nodejs
#define ERR_RET_ERROR_STR(str) \
{                                                       \
  printf("unimplemented ERR_RET_ERROR_STR %s\n", str);  \
  return false;                                         \
}

#define ERR_RET_ERROR_ERRNO(str) \
{                                                                       \
  fprintf (stderr, "%s error %d, %s\n", str, errno, strerror (errno));  \
  printf("unimplemented ERR_RET_ERROR_ERRNO %s\n", str);                \
  return false;                                                         \
}

#define ERR_RET_OK \
  return true;
#define ERR_RET_CHECK(ret)        \
  if (!ret) {                     \
    return false;                 \
  }

#define ERR_RET_CHECK_THREAD(ret)           \
  if (!ret) {                               \
    printf("[THREAD] capture stop fail\n"); \
    pthread_exit(0);                        \
  }

// not accessible for some reason, but we should not use it
#define EXIT_FAILURE 1

#define CLEAR(x) memset(&(x), 0, sizeof(x))  

////////////////////////////////////////////////////////////////////////////////////////////////////
//    data structures
////////////////////////////////////////////////////////////////////////////////////////////////////
enum Capture_io_method {
  IO_METHOD_READ,
  IO_METHOD_MMAP,
  IO_METHOD_USERPTR,
};

struct Capture_buffer {
  void   *start;
  size_t  length;
};

struct Capture_context {
  // nodejs aware of this
  char* video_dev_path;
  u32 ctx_idx;
  u32 size_x;
  u32 size_y;
  u32 fps;
  u32 fps_div;
  u32 px_format;
  // nodejs readable output
  volatile u64 frame_id;
  volatile bool capture_in_progress;
  volatile size_t frame_size;
  volatile u8* buffer;
  
  // nodejs probably aware of this
  bool live;
  bool need_shutdown;
  char* last_error; // for future
  bool last_error_need_free;
  
  // nodejs should be NOT aware of this
  pthread_t _thread_id;
  int       _fd;
  
  enum Capture_io_method  _io_type;
  
  struct Capture_buffer*  _buffers;
  u32                     _n_buffers;
  int                     _capture_frame_result;
};
////////////////////////////////////////////////////////////////////////////////////////////////////
//    misc
////////////////////////////////////////////////////////////////////////////////////////////////////
// should be not used
void errno_exit(const char *s) {
  fprintf (stderr, "%s error %d, %s\n",s, errno, strerror (errno));
  pthread_exit(0);
  // exit(EXIT_FAILURE);
}

int xioctl(int fd, int request, void *arg) {
  int r;
  
  do r = ioctl (fd, request, arg);
  while (-1 == r && EINTR == errno);
  
  return r;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
//    
//    internal stuff
//    
////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////
//    open/close
////////////////////////////////////////////////////////////////////////////////////////////////////
ERR_RET_TYPE _capture_open(struct Capture_context* ctx) {
  struct stat st;
  
  if (-1 == stat(ctx->video_dev_path, &st)) {
    fprintf(stderr, "Cannot identify '%s': %d, %s\n", ctx->video_dev_path, errno, strerror(errno));
    ERR_RET_ERROR
  }
  
  if (!S_ISCHR(st.st_mode)) {
    fprintf(stderr, "%s is no device\n", ctx->video_dev_path);
    ERR_RET_ERROR
  }
  
  printf("ctx->video_dev_path = %s\n", ctx->video_dev_path);
  ctx->_fd = open(ctx->video_dev_path, O_RDWR /* required */ | O_NONBLOCK, 0);
  
  if (-1 == ctx->_fd) {
    fprintf(stderr, "Cannot open '%s': %d, %s\n", ctx->video_dev_path, errno, strerror(errno));
    ERR_RET_ERROR
  }
  ERR_RET_OK
}
ERR_RET_TYPE _capture_close(struct Capture_context* ctx) {
  if (-1 == close(ctx->_fd))
    ERR_RET_ERROR
  
  ctx->_fd = -1;
  ERR_RET_OK
}
////////////////////////////////////////////////////////////////////////////////////////////////////
//    variations
////////////////////////////////////////////////////////////////////////////////////////////////////
ERR_RET_TYPE _capture_init_read(struct Capture_context* ctx, unsigned int buffer_size) {
  ctx->_buffers = (struct Capture_buffer*)calloc(1, sizeof(*ctx->_buffers));
  
  if (!ctx->_buffers) {
    fprintf(stderr, "Out of memory\n");
    // exit(EXIT_FAILURE);
    ERR_RET_ERROR
  }
  
  ctx->_buffers[0].length = buffer_size;
  ctx->_buffers[0].start = malloc(buffer_size);
  
  if (!ctx->_buffers[0].start) {
    fprintf(stderr, "Out of memory\n");
    // exit(EXIT_FAILURE);
    ERR_RET_ERROR
  }
  ERR_RET_OK
}

ERR_RET_TYPE _capture_init_mmap(struct Capture_context* ctx) {
  struct v4l2_requestbuffers req;
  
  CLEAR(req);
  
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  
  if (-1 == xioctl(ctx->_fd, VIDIOC_REQBUFS, &req)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s does not support memory mapping\n", ctx->video_dev_path);
      // exit(EXIT_FAILURE);
      ERR_RET_ERROR
    } else {
      ERR_RET_ERROR_ERRNO("VIDIOC_REQBUFS");
    }
  }
  
  if (req.count < 2) {
    fprintf(stderr, "Insufficient _capture_buffer memory on %s\n", ctx->video_dev_path);
    // exit(EXIT_FAILURE);
    ERR_RET_ERROR
  }
  
  ctx->_buffers = (struct Capture_buffer*)calloc(req.count, sizeof(*ctx->_buffers));
  
  if (!ctx->_buffers) {
    fprintf(stderr, "Out of memory\n");
    // exit(EXIT_FAILURE);
    ERR_RET_ERROR
  }
  
  for (ctx->_n_buffers = 0; ctx->_n_buffers < req.count; ++ctx->_n_buffers) {
    struct v4l2_buffer buf;
    
    CLEAR(buf);
    
    buf.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory      = V4L2_MEMORY_MMAP;
    buf.index       = ctx->_n_buffers;
    
    if (-1 == xioctl(ctx->_fd, VIDIOC_QUERYBUF, &buf))
      ERR_RET_ERROR_ERRNO("VIDIOC_QUERYBUF");
    
    ctx->_buffers[ctx->_n_buffers].length = buf.length;
    ctx->_buffers[ctx->_n_buffers].start =
      mmap(NULL /* start anywhere */,
            buf.length,
            PROT_READ | PROT_WRITE /* required */,
            MAP_SHARED /* recommended */,
            ctx->_fd, buf.m.offset);
    
    if (MAP_FAILED == ctx->_buffers[ctx->_n_buffers].start)
      ERR_RET_ERROR_ERRNO("mmap");
  }
  
  ERR_RET_OK
}

ERR_RET_TYPE _capture_init_userp(struct Capture_context* ctx, unsigned int buffer_size) {
  struct v4l2_requestbuffers req;
  
  CLEAR(req);
  
  req.count  = 4;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_USERPTR;
  
  if (-1 == xioctl(ctx->_fd, VIDIOC_REQBUFS, &req)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s does not support user pointer i/o\n", ctx->video_dev_path);
      // exit(EXIT_FAILURE);
      ERR_RET_ERROR
    } else {
      ERR_RET_ERROR_ERRNO("VIDIOC_REQBUFS");
    }
  }
  
  ctx->_buffers = (struct Capture_buffer*)calloc(4, sizeof(*ctx->_buffers));
  
  if (!ctx->_buffers) {
    fprintf(stderr, "Out of memory\n");
    // exit(EXIT_FAILURE);
    ERR_RET_ERROR
  }
  
  for (ctx->_n_buffers = 0; ctx->_n_buffers < 4; ++ctx->_n_buffers) {
    ctx->_buffers[ctx->_n_buffers].length = buffer_size;
    ctx->_buffers[ctx->_n_buffers].start = malloc(buffer_size);
    
    if (!ctx->_buffers[ctx->_n_buffers].start) {
      fprintf(stderr, "Out of memory\n");
      // exit(EXIT_FAILURE);
      ERR_RET_ERROR
    }
  }
  ERR_RET_OK
}
////////////////////////////////////////////////////////////////////////////////////////////////////
//    init/deinit
////////////////////////////////////////////////////////////////////////////////////////////////////
ERR_RET_TYPE _capture_init_device(struct Capture_context* ctx) {
  struct v4l2_capability cap;
  struct v4l2_cropcap cropcap;
  struct v4l2_crop crop;
  struct v4l2_format fmt;
  unsigned int min;
  
  if (-1 == xioctl(ctx->_fd, VIDIOC_QUERYCAP, &cap)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s is no V4L2 device\n", ctx->video_dev_path);
      // exit(EXIT_FAILURE);
      ERR_RET_ERROR
    } else {
      ERR_RET_ERROR_ERRNO("VIDIOC_QUERYCAP");
    }
  }
  
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "%s is no video capture device\n", ctx->video_dev_path);
    // exit(EXIT_FAILURE);
    ERR_RET_ERROR
  }
  
  switch (ctx->_io_type) {
    case IO_METHOD_READ:
      if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
        fprintf(stderr, "%s does not support read i/o\n", ctx->video_dev_path);
        // exit(EXIT_FAILURE);
        ERR_RET_ERROR
      }
      break;
    
    case IO_METHOD_MMAP:
    case IO_METHOD_USERPTR:
      if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s does not support streaming i/o\n", ctx->video_dev_path);
        // exit(EXIT_FAILURE);
        ERR_RET_ERROR
      }
      break;
  }
  
  /* Select video input, video standard and tune here. */
  
  CLEAR(cropcap);
  
  cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  
  if (0 == xioctl(ctx->_fd, VIDIOC_CROPCAP, &cropcap)) {
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    crop.c = cropcap.defrect; /* reset to default */
  
    if (-1 == xioctl(ctx->_fd, VIDIOC_S_CROP, &crop)) {
      switch (errno) {
        case EINVAL:
          /* Cropping not supported. */
          break;
        default:
          /* Errors ignored. */
          break;
      }
    }
  } else {
    /* Errors ignored. */
  }
  
  CLEAR(fmt);
  
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  // force_format
  
  fmt.fmt.pix.width       = ctx->size_x;
  fmt.fmt.pix.height      = ctx->size_y;
  switch(ctx->px_format) {
    case 0: fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;break;
    case 1: fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;break;
    
    default:fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
  }
  
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;
  
  if (-1 == xioctl(ctx->_fd, VIDIOC_S_FMT, &fmt))
    ERR_RET_ERROR_ERRNO("VIDIOC_S_FMT");
  
  /* Note VIDIOC_S_FMT may change width and height. */
  
  
  
  struct v4l2_streamparm parm;
  CLEAR(parm);
  
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  
  parm.parm.capture.timeperframe.numerator  = ctx->fps_div;
  parm.parm.capture.timeperframe.denominator= ctx->fps;
  
  if (-1 == xioctl(ctx->_fd, VIDIOC_S_PARM, &parm))
    ERR_RET_ERROR_ERRNO("VIDIOC_S_PARM");
  
  CLEAR(parm);
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(ctx->_fd, VIDIOC_G_PARM, &parm))
    ERR_RET_ERROR_ERRNO("VIDIOC_G_PARM");
  
  /* Buggy driver paranoia. */
  min = fmt.fmt.pix.width * 2;
  if (fmt.fmt.pix.bytesperline < min)
    fmt.fmt.pix.bytesperline = min;
  min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
  if (fmt.fmt.pix.sizeimage < min)
    fmt.fmt.pix.sizeimage = min;
  
  ERR_RET_TYPE ret;
  switch (ctx->_io_type) {
    case IO_METHOD_READ:
      ret = _capture_init_read(ctx, fmt.fmt.pix.sizeimage);
      ERR_RET_CHECK(ret)
      break;
    
    case IO_METHOD_MMAP:
      ret = _capture_init_mmap(ctx);
      ERR_RET_CHECK(ret)
      break;
    
    case IO_METHOD_USERPTR:
      ret = _capture_init_userp(ctx, fmt.fmt.pix.sizeimage);
      ERR_RET_CHECK(ret)
      break;
  }
  ERR_RET_OK
}

ERR_RET_TYPE _capture_uninit_device(struct Capture_context* ctx) {
  unsigned int i;
  
  switch (ctx->_io_type) {
    case IO_METHOD_READ:
      free(ctx->_buffers[0].start);
      break;
    
    case IO_METHOD_MMAP:
      for (i = 0; i < ctx->_n_buffers; ++i)
        if (-1 == munmap(ctx->_buffers[i].start, ctx->_buffers[i].length))
          ERR_RET_ERROR_ERRNO("munmap")
      break;
    
    case IO_METHOD_USERPTR:
      for (i = 0; i < ctx->_n_buffers; ++i)
        free(ctx->_buffers[i].start);
      break;
  }
  
  free(ctx->_buffers);
  ERR_RET_OK
}
////////////////////////////////////////////////////////////////////////////////////////////////////
//    mid-level start/stop
////////////////////////////////////////////////////////////////////////////////////////////////////
ERR_RET_TYPE _capture_start(struct Capture_context* ctx) {
  unsigned int i;
  enum v4l2_buf_type type;
  
  switch (ctx->_io_type) {
  case IO_METHOD_READ:
    /* Nothing to do. */
    break;
  
  case IO_METHOD_MMAP:
    for (i = 0; i < ctx->_n_buffers; ++i) {
      struct v4l2_buffer buf;
      
      CLEAR(buf);
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      
      if (-1 == xioctl(ctx->_fd, VIDIOC_QBUF, &buf))
        ERR_RET_ERROR_ERRNO("VIDIOC_QBUF");
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(ctx->_fd, VIDIOC_STREAMON, &type))
      ERR_RET_ERROR_ERRNO("VIDIOC_STREAMON");
    break;
  
  case IO_METHOD_USERPTR:
    for (i = 0; i < ctx->_n_buffers; ++i) {
      struct v4l2_buffer buf;
      
      CLEAR(buf);
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_USERPTR;
      buf.index = i;
      buf.m.userptr = (unsigned long)ctx->_buffers[i].start;
      buf.length = ctx->_buffers[i].length;
      
      if (-1 == xioctl(ctx->_fd, VIDIOC_QBUF, &buf))
        ERR_RET_ERROR_ERRNO("VIDIOC_QBUF");
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(ctx->_fd, VIDIOC_STREAMON, &type))
      ERR_RET_ERROR_ERRNO("VIDIOC_STREAMON");
    break;
  }
  
  ERR_RET_OK
}

ERR_RET_TYPE _capture_stop(struct Capture_context* ctx) {
  enum v4l2_buf_type type;

  switch (ctx->_io_type) {
  case IO_METHOD_READ:
    /* Nothing to do. */
    break;

  case IO_METHOD_MMAP:
  case IO_METHOD_USERPTR:
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(ctx->_fd, VIDIOC_STREAMOFF, &type))
      ERR_RET_ERROR_ERRNO("VIDIOC_STREAMOFF");
    break;
  }
  
  ERR_RET_OK
}

////////////////////////////////////////////////////////////////////////////////////////////////////
//    mid-level capture
////////////////////////////////////////////////////////////////////////////////////////////////////

ERR_RET_TYPE _capture_process_image(struct Capture_context* ctx, const void *p, size_t size) {
  // TODO capture time to ctx
  // long long ts = now();
  // printf("%lld capture_process_image %d %lld\n", capture_frame_id, size, (ts - capture_last_frame_ts)/1000);
  // capture_frame_id++;
  // capture_last_frame_ts = ts;
  // 
  // save_to_disk(p, size);
  
  ctx->capture_in_progress = true;
  ctx->frame_size = size;
  // printf("size = %d %d\n", size, ctx->frame_id);
  
  // prevent accidental buffer overflow
  // size = min(size, 3*ctx->size_x*ctx->size_y);
  if (size > 3*ctx->size_x*ctx->size_y)
    size = 3*ctx->size_x*ctx->size_y;
  
  memcpy((u8*)ctx->buffer, p, size);
  ctx->frame_id++;
  ctx->capture_in_progress = false;
  ERR_RET_OK
}

ERR_RET_TYPE _capture_read_frame(struct Capture_context* ctx) {
  struct v4l2_buffer buf;
  unsigned int i;

  switch (ctx->_io_type) {
    case IO_METHOD_READ:
      if (-1 == read(ctx->_fd, ctx->_buffers[0].start, ctx->_buffers[0].length)) {
        switch (errno) {
        case EAGAIN:
          ctx->_capture_frame_result = 0;
          ERR_RET_OK
        
        case EIO:
          /* Could ignore EIO, see spec. */
          
          /* fall through */
        
        default:
          ERR_RET_ERROR_ERRNO("read");
        }
      }
      
      _capture_process_image(ctx, ctx->_buffers[0].start, ctx->_buffers[0].length);
      break;
    
    case IO_METHOD_MMAP:
      CLEAR(buf);
      
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      
      if (-1 == xioctl(ctx->_fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
        case EAGAIN:
          ctx->_capture_frame_result = 0;
          ERR_RET_OK
        
        case EIO:
          /* Could ignore EIO, see spec. */
          
          /* fall through */
        
        default:
          ERR_RET_ERROR_ERRNO("VIDIOC_DQBUF");
        }
      }
      
      assert(buf.index < ctx->_n_buffers);
      
      _capture_process_image(ctx, ctx->_buffers[buf.index].start, buf.bytesused);
      
      if (-1 == xioctl(ctx->_fd, VIDIOC_QBUF, &buf))
        ERR_RET_ERROR_ERRNO("VIDIOC_QBUF");
      break;
    
    case IO_METHOD_USERPTR:
      CLEAR(buf);
      
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_USERPTR;
      
      if (-1 == xioctl(ctx->_fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
        case EAGAIN:
          ctx->_capture_frame_result = 0;
          ERR_RET_OK
        
        case EIO:
          /* Could ignore EIO, see spec. */
          
          /* fall through */
        
        default:
          ERR_RET_ERROR_ERRNO("VIDIOC_DQBUF");
        }
      }
      
      for (i = 0; i < ctx->_n_buffers; ++i)
        if (buf.m.userptr == (unsigned long)ctx->_buffers[i].start && buf.length == ctx->_buffers[i].length)
          break;
      
      assert(i < ctx->_n_buffers);
      
      _capture_process_image(ctx, (void *)buf.m.userptr, buf.bytesused);
      
      if (-1 == xioctl(ctx->_fd, VIDIOC_QBUF, &buf))
        ERR_RET_ERROR_ERRNO("VIDIOC_QBUF");
      break;
  }
  
  ctx->_capture_frame_result = 1;
  ERR_RET_OK
}
////////////////////////////////////////////////////////////////////////////////////////////////////
//    thread stuff
////////////////////////////////////////////////////////////////////////////////////////////////////
static void* _capture_main_loop(void* arg) {
  struct Capture_context* ctx = (struct Capture_context*)arg;
  printf("[THREAD] capture start\n");
  ctx->live = true;
  while (!ctx->need_shutdown) {
    for (;;) {
      fd_set fds;
      struct timeval tv;
      int r;
      
      FD_ZERO(&fds);
      FD_SET(ctx->_fd, &fds);
      
      /* Timeout. */
      tv.tv_sec = 2;
      tv.tv_usec = 0;
      
      r = select(ctx->_fd + 1, &fds, NULL, NULL, &tv);
      
      if (-1 == r) {
        if (EINTR == errno)
          continue;
        // errno_exit("select");
        fprintf(stderr, "select wtf\n");
        break;
      }
      
      if (0 == r) {
        fprintf(stderr, "select timeout\n");
        // exit(EXIT_FAILURE);
        break;
      }
      
      ERR_RET_TYPE ret = _capture_read_frame(ctx);
      if (!ret) {
        // TODO print error
        ctx->need_shutdown = true;
        break;
      }
      if (ctx->_capture_frame_result)
        break;
      /* EAGAIN - continue select loop. */
    }
  }
  
  ctx->live = false;
  
  ERR_RET_TYPE ret;
  ret = _capture_stop(ctx);
  ERR_RET_CHECK_THREAD(ret)
  
  ret = _capture_uninit_device(ctx);
  ERR_RET_CHECK_THREAD(ret)
  
  ret = _capture_close(ctx);
  ERR_RET_CHECK_THREAD(ret)
  
  printf("[THREAD] capture stop ok\n");
  
  pthread_exit(0);
}
////////////////////////////////////////////////////////////////////////////////////////////////////
//    API
////////////////////////////////////////////////////////////////////////////////////////////////////
ERR_RET_TYPE capture_start(struct Capture_context* ctx) {
  ERR_RET_TYPE ret;
  ctx->live = true;
  
  ret = _capture_open(ctx);
  ERR_RET_CHECK(ret)
  
  ret = _capture_init_device(ctx);
  ERR_RET_CHECK(ret)
  
  ret = _capture_start(ctx);
  ERR_RET_CHECK(ret)
  
  int err_code = pthread_create(&ctx->_thread_id, 0, _capture_main_loop, ctx);
  if (err_code) {
    fprintf(stderr, "capture pthread_create fail err_code = %d", err_code);
    ret = _capture_stop(ctx);
    ERR_RET_CHECK(ret)
    ERR_RET_ERROR
  }
  ERR_RET_OK
}

ERR_RET_TYPE capture_stop(struct Capture_context* ctx) {
  // ERR_RET_TYPE ret;
  if (!ctx->live) ERR_RET_OK; // not really ok
  if (ctx->need_shutdown) ERR_RET_OK; // not really ok
  ctx->need_shutdown = true;
  
  pthread_join(ctx->_thread_id, NULL);
  
  ERR_RET_OK
}
