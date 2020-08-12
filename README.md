# napi_v4l2
TODO make good readme\
only usage example for now

    #!/usr/bin/env node
    mod = require("napi_v4l2")
    fs = require("fs")
    napi_png = require("napi_png")
    video_dev_path = "/dev/video0"
    size_x = 1920
    size_y = 1080
    fps    = 60
    fps_div= 1
    px_format = 0 // 0 BGR 1 RGB(experimental)
    
    // NOTE always returns 0 (for now)
    buffer = Buffer.alloc(3*size_x*size_y)
    png_buffer = Buffer.alloc(3*size_x*size_y)
    
    ctx = mod.start(video_dev_path, size_x, size_y, fps, fps_div, px_format)
    console.log("started");
    
    last_frame_idx = -1
    interval = setInterval(()=>{
      // NOTE if internal frame_idx same as previous returned last_frame_idx then there will be no copy
      [curr_frame_idx, frame_size, capture_ok] = mod.frame_get(ctx, buffer, last_frame_idx)
      if (curr_frame_idx == 0) return;
      if (!capture_ok) {
        console.log("!capture_ok", curr_frame_idx)
      } else if (frame_size != buffer.length) {
        console.log("frame_size != buffer.length", frame_size, buffer.length, curr_frame_idx)
      } else if (last_frame_idx != curr_frame_idx) {
        last_frame_idx = curr_frame_idx;
        // console.log("good frame", curr_frame_idx);
        res = napi_png.png_encode_rgb(buffer, size_x, size_y, png_buffer, 0);
        dst_offset_start  = res[0]
        dst_offset_end    = res[1]
        res_png_dst       = res[2]
        fs.writeFileSync("test_encode.png", res_png_dst.slice(dst_offset_start, dst_offset_end));
      }
    }, 1);
    
    setTimeout(()=>{
      clearInterval(interval);
      mod.stop(ctx);
      console.log("stopped");
    }, 1000)
 
