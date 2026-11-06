# SPDX-License-Identifier: GPL-3.0-or-later
"""Render the real upstream GUI at 640x480 on Zero28's portrait framebuffer.

Keep upstream's accelerated texture drawing, adding the same centered 90-degree
presentation used by the native CloveOS frontend. Landscape displays retain 0°.
"""
import ctypes
import sdl2
import sdl2.ext


class CloveRenderer(sdl2.ext.Renderer):
    def __init__(self, window):
        try:
            super().__init__(window, logical_size=(640,480), flags=sdl2.SDL_RENDERER_ACCELERATED | sdl2.SDL_RENDERER_TARGETTEXTURE)
        except sdl2.ext.SDLError:
            super().__init__(window, logical_size=(640,480), flags=sdl2.SDL_RENDERER_SOFTWARE | sdl2.SDL_RENDERER_TARGETTEXTURE)
        self.clove_target = sdl2.SDL_CreateTexture(
            self.sdlrenderer, sdl2.SDL_PIXELFORMAT_ARGB8888,
            sdl2.SDL_TEXTUREACCESS_TARGET, 640, 480)
        if not self.clove_target:
            raise RuntimeError(sdl2.SDL_GetError())
        sdl2.SDL_SetTextureBlendMode(self.clove_target, sdl2.SDL_BLENDMODE_NONE)
        sdl2.SDL_SetRenderTarget(self.sdlrenderer, self.clove_target)

    def present(self):
        if not getattr(self, 'clove_target', None):
            return super().present()
        raw = self.sdlrenderer
        sdl2.SDL_SetRenderTarget(raw, None)
        sdl2.SDL_RenderSetLogicalSize(raw, 0, 0)
        width, height = ctypes.c_int(), ctypes.c_int()
        sdl2.SDL_GetRendererOutputSize(raw, width, height)
        portrait = height.value > width.value
        scale = min(width.value / (480 if portrait else 640),
                    height.value / (640 if portrait else 480))
        w, h = round(640 * scale), round(480 * scale)
        target = sdl2.SDL_Rect((width.value-w)//2, (height.value-h)//2, w, h)
        sdl2.SDL_RenderClear(raw)
        if sdl2.SDL_RenderCopyEx(raw, self.clove_target, None, target,
                               90.0 if portrait else 0.0, None, sdl2.SDL_FLIP_NONE) != 0:
            raise RuntimeError(sdl2.SDL_GetError())
        sdl2.SDL_RenderPresent(raw)
        sdl2.SDL_SetRenderTarget(raw, self.clove_target)
        sdl2.SDL_RenderSetLogicalSize(raw, 640, 480)

    def destroy(self):
        if getattr(self, 'clove_target', None):
            sdl2.SDL_SetRenderTarget(self.sdlrenderer, None)
            sdl2.SDL_DestroyTexture(self.clove_target)
            self.clove_target = None
        return super().destroy()
