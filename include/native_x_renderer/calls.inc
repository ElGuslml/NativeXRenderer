// NativeXRenderer - the XDK Direct3D entry points a game can report (see hooks.h).
//
// NATIVE_X_RENDERER_CALL(id, arguments, flags)
//   id         Call::k<id>. Names follow the XDK API.
//   arguments  What the trace prints, one name per argument register from r3: "dev" marks the D3D
//              device; a name starting with '.' is a float argument, printed from f1 (it still takes
//              its integer register slot, as in the Xbox 360 calling convention).
//   flags      kDraw: the trace records the draw state from the device; kFrameEnd: the frame ends
//              when the outermost call of this kind returns; kPackets: the call writes packets into the
//              device's command buffer although its first argument is not the device (the state
//              tracker reads them).
//
// Not every game uses every entry point. A game hooks the ones its executable has.

// Draws
NATIVE_X_RENDERER_CALL(DrawIndexedVertices, "dev prim baseVertex startIndex indexCount", kDraw)
NATIVE_X_RENDERER_CALL(DrawVertices, "dev prim startVertex vertexCount", kDraw)
NATIVE_X_RENDERER_CALL(DrawVerticesUP, "dev prim vertexCount data stride", kDraw)
NATIVE_X_RENDERER_CALL(BeginVertices, "dev prim vertexCount stride", kDraw)
NATIVE_X_RENDERER_CALL(EndVertices, "dev", 0)

// Render targets, clears, resolves, tiling
NATIVE_X_RENDERER_CALL(SetRenderTarget, "dev index surface", 0)
NATIVE_X_RENDERER_CALL(SetDepthStencilSurface, "dev surface", 0)
NATIVE_X_RENDERER_CALL(Clear, "dev count rects flags color .z stencil", 0)
NATIVE_X_RENDERER_CALL(ClearF, "dev flags rect color .z stencil", 0)
NATIVE_X_RENDERER_CALL(Resolve, "dev flags srcRect destTexture destPoint level slice clearColor .z", 0)
NATIVE_X_RENDERER_CALL(BeginTiling, "dev flags count tileRects clearColor .z stencil", 0)
NATIVE_X_RENDERER_CALL(EndTiling, "dev resolveFlags resolveRects destTexture clearColor .z stencil params", 0)
NATIVE_X_RENDERER_CALL(BeginZPass, "dev flags", 0)
NATIVE_X_RENDERER_CALL(EndZPass, "dev", 0)
NATIVE_X_RENDERER_CALL(SetPredication, "dev mask", 0)
NATIVE_X_RENDERER_CALL(SetViewport, "dev viewport", 0)
NATIVE_X_RENDERER_CALL(SetScissorRect, "dev rect", 0)

// Shaders, vertex input, textures, constants
NATIVE_X_RENDERER_CALL(SetVertexShader, "dev shader", 0)
NATIVE_X_RENDERER_CALL(SetPixelShader, "dev shader", 0)
NATIVE_X_RENDERER_CALL(SetVertexDeclaration, "dev declaration", 0)
NATIVE_X_RENDERER_CALL(SetStreamSource, "dev stream buffer offset stride", 0)
NATIVE_X_RENDERER_CALL(SetIndices, "dev buffer", 0)
NATIVE_X_RENDERER_CALL(SetTexture, "dev sampler texture", 0)
NATIVE_X_RENDERER_CALL(SetVertexShaderConstantF, "dev start data count", 0)
NATIVE_X_RENDERER_CALL(SetPixelShaderConstantF, "dev start data count", 0)

// Resources
NATIVE_X_RENDERER_CALL(VertexBufferLock, "buffer offset size data flags", 0)
NATIVE_X_RENDERER_CALL(VertexBufferUnlock, "buffer", 0)
NATIVE_X_RENDERER_CALL(IndexBufferLock, "buffer offset size data flags", 0)
NATIVE_X_RENDERER_CALL(IndexBufferUnlock, "buffer", 0)
NATIVE_X_RENDERER_CALL(TextureUnlock, "texture", 0)
NATIVE_X_RENDERER_CALL(FlushCpuRange, "start end", 0)

// Command buffer, queries, frame
NATIVE_X_RENDERER_CALL(KickSegment, "dev", 0)
// Moves the write pointer to a new command buffer segment. KickSegment calls it, and so may the
// library's reservation, when what it reserves does not fit even after a kick.
NATIVE_X_RENDERER_CALL(NewSegment, "dev dwords", 0)
NATIVE_X_RENDERER_CALL(SetShaderGPRAllocation, "dev flags vertexGprs pixelGprs", 0)
NATIVE_X_RENDERER_CALL(QueryIssue, "query flags", kPackets)
NATIVE_X_RENDERER_CALL(QueryGetData, "query data size flags", 0)
NATIVE_X_RENDERER_CALL(Swap, "dev frontBuffer params", 0)
NATIVE_X_RENDERER_CALL(Present, "dev params", kFrameEnd)
