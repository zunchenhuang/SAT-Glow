#include "recorder.hpp"

#include <glad/glad.h>

#include <cstdlib>
#include <sys/stat.h>

static bool ends_with (const std::string &s, const char *suf) {
  const std::string t (suf);
  return s.size () >= t.size () && !s.compare (s.size () - t.size (), t.size (), t);
}

static std::string quote (const std::string &s) {
  std::string r = "'";
  for (char c : s)
    r += c == '\'' ? std::string ("'\\''") : std::string (1, c);
  return r + "'";
}

std::string next_free_name (const char *prefix, const char *ext) {
  char name[256];
  struct stat st;
  for (int i = 0; i < 1000; i++) {
    snprintf (name, sizeof name, "%s-%03d.%s", prefix, i, ext);
    if (stat (name, &st) != 0)
      return name;
  }
  snprintf (name, sizeof name, "%s-overflow.%s", prefix, ext);
  return name;
}

bool Recorder::start (const std::string &path, int w, int h, int fps, std::string &err) {
  stop ();
  if (system ("command -v ffmpeg > /dev/null 2>&1") != 0) {
    err = "ffmpeg not found (sudo apt install ffmpeg)";
    return false;
  }
  // Frames arrive bottom-up from OpenGL; flip them and crop to even sizes,
  // which H.264 needs.
  const std::string base = "vflip,crop=trunc(iw/2)*2:trunc(ih/2)*2";
  std::string out;
  if (ends_with (path, ".gif"))
    out = "-filter_complex \"" + base +
          ",fps=15,scale='min(800,iw)':-2:flags=lanczos,split[a][b];[a]palettegen=max_colors=192:stats_mode=diff[p];"
          "[b][p]paletteuse=dither=bayer:bayer_scale=4\" -loop 0";
  else if (ends_with (path, ".webm"))
    out = "-vf '" + base + "' -c:v libvpx-vp9 -b:v 0 -crf 32 -row-mt 1 -pix_fmt yuv420p";
  else
    out = "-vf '" + base + "' -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -movflags +faststart";
  char head[256];
  snprintf (head, sizeof head, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i - ", w, h, fps);
  const std::string cmd = std::string (head) + out + " " + quote (path);
  pipe_ = popen (cmd.c_str (), "w");
  if (!pipe_) {
    err = "cannot start ffmpeg";
    return false;
  }
  w_ = w;
  h_ = h;
  fps_ = fps;
  frames_ = 0;
  path_ = path;
  buf_.resize ((size_t) w * (size_t) h * 3);
  return true;
}

void Recorder::frame (int copies) {
  if (!pipe_ || copies <= 0)
    return;
  glPixelStorei (GL_PACK_ALIGNMENT, 1);
  glReadBuffer (GL_BACK);
  glReadPixels (0, 0, w_, h_, GL_RGB, GL_UNSIGNED_BYTE, buf_.data ());
  for (int i = 0; i < copies; i++) {
    if (fwrite (buf_.data (), 1, buf_.size (), pipe_) != buf_.size ()) {
      fprintf (stderr, "satglow: ffmpeg stopped accepting frames, recording ended\n");
      stop ();
      return;
    }
    frames_++;
  }
}

void Recorder::stop () {
  if (!pipe_)
    return;
  const int rc = pclose (pipe_);
  pipe_ = nullptr;
  if (rc == 0)
    printf ("satglow: wrote %s (%ld frames, %.1f s)\n", path_.c_str (), frames_, (double) frames_ / fps_);
  else
    fprintf (stderr, "satglow: ffmpeg reported an error while writing %s\n", path_.c_str ());
}
