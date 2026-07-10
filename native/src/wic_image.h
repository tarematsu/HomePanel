#pragma once

#include "common.h"

namespace hp {
inline HBITMAP WicSourceToDibSection(IWICImagingFactory* factory, IWICBitmapSource* source,
                                     int width, int height) {
  if (!factory || !source || width <= 0 || height <= 0) return nullptr;
  ComPtr<IWICBitmapScaler> scaler;
  if (FAILED(factory->CreateBitmapScaler(&scaler))) return nullptr;
  if (FAILED(scaler->Initialize(source, static_cast<UINT>(width), static_cast<UINT>(height),
                                WICBitmapInterpolationModeFant))) {
    return nullptr;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter))) return nullptr;
  if (FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return nullptr;
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!bitmap || !pixels) {
    if (bitmap) DeleteObject(bitmap);
    return nullptr;
  }
  const UINT stride = static_cast<UINT>(width) * 4;
  if (FAILED(converter->CopyPixels(nullptr, stride, stride * static_cast<UINT>(height),
                                   static_cast<BYTE*>(pixels)))) {
    DeleteObject(bitmap);
    return nullptr;
  }
  return bitmap;
}

inline HBITMAP DecodeImageFileToBitmap(const fs::path& file, int width, int height) {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder))) {
    return nullptr;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
  return WicSourceToDibSection(factory.Get(), frame.Get(), width, height);
}

inline HBITMAP DecodeImageBytesToBitmap(const void* data, size_t size, int width, int height) {
  if (!data || !size || size > UINT32_MAX) return nullptr;
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }
  ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(static_cast<const BYTE*>(data), static_cast<UINT>(size)));
  if (!stream) return nullptr;
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                              WICDecodeMetadataCacheOnDemand, &decoder))) {
    return nullptr;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
  return WicSourceToDibSection(factory.Get(), frame.Get(), width, height);
}
}
