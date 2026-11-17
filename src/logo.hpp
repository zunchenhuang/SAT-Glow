// The SATGlow wordmark as a triangle mesh.
//
// Every letter is a ribbon of triangles along a centre line.  The same mesh
// is used for the start-up animation, the small logo in the side panel and
// the SVG written by `satglow --logo-svg`.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct LogoMesh {
  // Vertices in logo units: baseline at y = 0, capital height 1, y up.
  std::vector<float> vx, vy;
  std::vector<uint32_t> tri; // three vertex indices per triangle
  std::vector<float> rgb;    // final colour, three floats per triangle
  float width = 0, height = 0;
  size_t ntriangles () const { return tri.size () / 3; }
};

void build_logo (LogoMesh &mesh);

// Writes the logo as a stand-alone SVG on a dark rounded card.
bool write_logo_svg (const LogoMesh &mesh, const std::string &path);
