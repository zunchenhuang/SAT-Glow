// Video and GIF export.  Frames are read back from the window and piped
// into ffmpeg, which must be installed (sudo apt install ffmpeg).  The file
// extension picks the format: .mp4 (H.264), .webm (VP9) or .gif.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

class Recorder {
public:
  ~Recorder () { stop (); }

  // Starts a recording of a w x h framebuffer.  Returns false and sets
  // `err` if ffmpeg is missing or cannot be started.
  bool start (const std::string &path, int w, int h, int fps, std::string &err);

  // Reads the current back buffer and sends it `copies` times (to keep the
  // video in step with real time when frames are slow).
  void frame (int copies = 1);

  // Finishes the file (waits for ffmpeg).
  void stop ();

  bool active () const { return pipe_ != nullptr; }
  int width () const { return w_; }
  int height () const { return h_; }
  int fps () const { return fps_; }
  long frames () const { return frames_; }
  const std::string &path () const { return path_; }

private:
  FILE *pipe_ = nullptr;
  int w_ = 0, h_ = 0, fps_ = 30;
  long frames_ = 0;
  std::string path_;
  std::vector<unsigned char> buf_;
};

// Next free name like satglow-000.mp4 in the current directory.
std::string next_free_name (const char *prefix, const char *ext);
