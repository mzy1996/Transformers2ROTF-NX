// T2ROTF - WMV movies decoded natively with FFmpeg
//
// The game's videos (Movies/*.wmv, WVC1/VC-1 Advanced Profile for the intro and
// WMV3 for the rest) hang the recompiled XDK decoder on the Switch: the video
// thread spins at ~99 % of a core at 0 FPS (see rex_perfil logs, "JQ Worker 0").
// On the PC the XDK decoder is fast enough and plays WMV3, but WVC1 (VC-1
// Advanced Profile + B frames) never plays: the game shows a black screen at
// 0 fps while the worker spins.
//  - DescodificadorWmv3: decodes with FFmpeg's WMV3/VC-1 the compressed frames
//    as stored in the container, without reordering: each frame comes out of
//    the call that decodes it, also with B frames.
//  - LeerInfoWmv: reads the size and the 4 WMV3 sequence bytes from the movie's
//    ASF header.
//  - PeliculaWmv: full ASF reader, decodes the file on its own (the fallback
//    strategy used by t2rotf_video_nativo.cpp, because the T2ROTF decoder has
//    no per-chunk data callback like the NFSMW one).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace t2rotf::video_wmv3 {

// YUV 4:2:0 planes of the last decoded frame; valid until the next call.
struct Fotograma {
  const uint8_t* planos[3] = {nullptr, nullptr, nullptr};
  int pasos[3] = {0, 0, 0};
  int ancho = 0;
  int alto = 0;
  bool clave = false;
  uint32_t bytes = 0;  // compressed frame size
};

struct InfoWmv {
  int ancho = 0;
  int alto = 0;
  std::vector<uint8_t> secuencia;  // extra data of the WMV3/WVC1 stream (STRUCT_C)
  uint32_t fourcc = 0;             // 'WMV3' or 'WVC1'
};

// Reads the movie's ASF header using the guest path (the same one NtCreateFile uses).
bool LeerInfoWmv(const std::string& ruta, InfoWmv& info);

class DescodificadorWmv3 {
 public:
  DescodificadorWmv3();
  ~DescodificadorWmv3();
  DescodificadorWmv3(const DescodificadorWmv3&) = delete;
  DescodificadorWmv3& operator=(const DescodificadorWmv3&) = delete;

  bool Abrir(const InfoWmv& info);
  // Decodes one complete compressed frame. false if FFmpeg fails or no picture.
  bool Descodificar(const uint8_t* datos, size_t bytes, bool clave, Fotograma& salida);

  int ancho() const { return ancho_; }
  int alto() const { return alto_; }
  int64_t fotogramas() const { return fotogramas_; }

 private:
  struct Estado;
  std::unique_ptr<Estado> e_;
  int ancho_ = 0;
  int alto_ = 0;
  int64_t fotogramas_ = 0;
};

class PeliculaWmv {
 public:
  PeliculaWmv();
  ~PeliculaWmv();
  PeliculaWmv(const PeliculaWmv&) = delete;
  PeliculaWmv& operator=(const PeliculaWmv&) = delete;

  // Opens the movie with the guest path and prepares the decoder.
  bool Abrir(const std::string& ruta);
  // Reads the next video frame from the container and decodes it. false at the end or on failure.
  bool Siguiente(Fotograma& salida);

  const std::string& ruta() const { return ruta_; }
  int ancho() const { return info_.ancho; }
  int alto() const { return info_.alto; }
  int64_t fotogramas() const { return fotogramas_; }

 private:
  struct Estado;
  std::unique_ptr<Estado> e_;
  std::string ruta_;
  InfoWmv info_;
  int64_t fotogramas_ = 0;
};

}  // namespace t2rotf::video_wmv3