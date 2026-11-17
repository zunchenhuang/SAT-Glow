// Small OpenGL helpers shared by the renderer and the intro.
#pragma once
#include <glad/glad.h>

#include <cstdio>
#include <cstdlib>

inline GLuint compile_shader (GLenum type, const char *src) {
  GLuint s = glCreateShader (type);
  glShaderSource (s, 1, &src, nullptr);
  glCompileShader (s);
  GLint ok = 0;
  glGetShaderiv (s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog (s, sizeof log, nullptr, log);
    fprintf (stderr, "satglow: shader compile error:\n%s\n", log);
    exit (1);
  }
  return s;
}

inline GLuint make_program (const char *vs, const char *fs) {
  GLuint p = glCreateProgram ();
  GLuint a = compile_shader (GL_VERTEX_SHADER, vs);
  GLuint b = compile_shader (GL_FRAGMENT_SHADER, fs);
  glAttachShader (p, a);
  glAttachShader (p, b);
  glLinkProgram (p);
  GLint ok = 0;
  glGetProgramiv (p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog (p, sizeof log, nullptr, log);
    fprintf (stderr, "satglow: shader link error:\n%s\n", log);
    exit (1);
  }
  glDeleteShader (a);
  glDeleteShader (b);
  return p;
}
