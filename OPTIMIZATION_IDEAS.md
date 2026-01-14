# CPU Efficiency Optimization Ideas

This document contains optimization ideas and analysis for improving CPU efficiency of the MOD player application. These are ideas for future reference - implementation requires careful testing.

## Key Findings

### Current Performance Issues
- Full framebuffer rotation (768KB) on every UI update
- CPU-bound framebuffer fills and rectangles
- Font rendering in tight loops
- Memory operations (memcpy/memset) not hardware-accelerated

### Target Hardware (ESP32-P4)
- **PPA (Pixel Processing Accelerator)**: Hardware accelerator for graphics operations
  - Scale, Rotate, Mirror (SRM) operations
  - Fill operations
  - Supports partial region operations
- **GDMA**: General DMA for memory-to-memory transfers
- **IRAM**: Fast internal RAM (~512KB total) for hot code
- **PSRAM**: Slower external RAM for large buffers

## Optimization Ideas (Ordered by Impact)

### 1. Partial PPA Rotation Instead of Full Screen
**Impact**: VERY HIGH (80-95% reduction in rotation overhead)
- **Location**: `main/main.c:blit()` - called after every tracker row update
- **Current Issue**: Rotates entire 800x480 framebuffer (768KB) on every update, even for single-row changes
- **Solution**: 
  - Track changed row regions
  - Use PPA SRM with `block_offset_x/y` to rotate only changed areas (~28KB instead of 768KB)
  - Use `esp_lcd_panel_draw_bitmap()` with partial region updates
- **Estimated CPU Savings**: 80-95% reduction in rotation overhead (from 768KB to ~28KB per row update)
- **Complexity**: Medium-High (requires coordinate transformation math)

### 2. Replace `fb_fill()` with PPA Fill
**Impact**: VERY HIGH (40-60% reduction in fill operations)
- **Location**: `main/main.c` - framebuffer operations, called frequently for clearing regions
- **Current Issue**: CPU-bound fill operations
- **Solution**: Use `ppa_do_fill()` with PPA_OPERATION_FILL
- **Estimated CPU Savings**: 40-60% reduction in fill operations
- **Complexity**: Low (straightforward API replacement)

### 3. Use PPA Fill for Rectangle Operations
**Impact**: HIGH (40-60% reduction in rectangle operations)
- **Location**: `main/main.c` - rectangle drawing
- **Current Issue**: CPU-bound rectangle drawing
- **Solution**: Replace `fb_rect()` calls with PPA Fill
- **Estimated CPU Savings**: 40-60% reduction in rectangle operations
- **Complexity**: Low (straightforward API replacement)

### 4. Move Hot Code to IRAM
**Impact**: HIGH (15-25% improvement in function execution speed)
- **Location**: Critical functions like `font_draw_string_scaled()`, `blit()`, etc.
- **Current Issue**: Hot code executing from slower flash/PSRAM
- **Solution**: Use `IRAM_ATTR` macro on hot functions
- **Constraints**: 
  - ESP32-P4 has ~512KB IRAM total (monitor with `idf.py size-components`)
  - **CRITICAL**: Functions marked IRAM_ATTR cannot use FreeRTOS APIs (semaphores, queues, blocking operations)
- **Estimated CPU Savings**: 15-25% improvement in hot code execution
- **Complexity**: Low-Medium (must be careful about FreeRTOS API usage)

### 5. GDMA for Memory Operations
**Impact**: HIGH (20-40% reduction in memory operation time)
- **Location**: `main/main.c:scroll_framebuffer_ppa()` - memcpy operations
- **Current Issue**: CPU-bound memory operations
- **Solution**: Use `esp_async_memcpy()` for large memory transfers
- **Warning**: 
  - Requires careful handling of IRAM_ATTR - FreeRTOS semaphores cannot be called from IRAM
  - If function is IRAM_ATTR, must use blocking or non-blocking without semaphores
- **Estimated CPU Savings**: 20-40% reduction in memory operation time
- **Complexity**: Medium (requires async callback handling)

### 6. Font Rendering Optimizations
**Impact**: MEDIUM-HIGH (10-20% reduction in font rendering time)
- **Location**: `main/simple_font.c:font_draw_string_scaled()`
- **Current Issue**: Font rendering is CPU-intensive with nested loops
- **Solution Options**:
  - Cache frequently used strings
  - Use lookup tables for character conversion (e.g., effect characters)
  - Optimize pixel writing loops
- **Estimated CPU Savings**: 10-20% reduction in font rendering time
- **Complexity**: Low-Medium

### 7. Optimize Mono-to-Stereo Conversion
**Impact**: MEDIUM (5-10% reduction in audio task CPU usage)
- **Location**: `main/mod_player.c` - audio buffer conversion
- **Current Issue**: CPU loop converts 4096 mono samples to stereo (4096 iterations)
- **Solution Options**:
  - DMA scatter-gather for pattern replication
  - Pre-allocated stereo buffer with stride memcpy
  - SIMD/NEON-style operations (if ESP32-P4 supports it)
- **Estimated CPU Savings**: 5-10% reduction in audio task CPU usage
- **Complexity**: Medium-High

### 8. PPA Async Mode
**Impact**: MEDIUM (allows overlapping operations)
- **Location**: PPA operations throughout
- **Current Issue**: PPA operations block CPU
- **Solution**: Use `PPA_TRANS_MODE_NON_BLOCKING` with callbacks
- **Benefits**: Overlap PPA operations with CPU work (pipeline processing)
- **Complexity**: Higher - requires callback management and queue handling
- **Estimated CPU Savings**: Variable - depends on how much work can be overlapped

## Implementation Notes

### Critical Constraints

1. **IRAM_ATTR Limitations**:
   - Functions marked `IRAM_ATTR` **CANNOT** use FreeRTOS APIs
   - No semaphores, queues, or blocking operations
   - Must be careful when combining with async operations
   - Example: If `scroll_framebuffer_ppa()` is IRAM_ATTR, cannot use `xSemaphoreCreateBinary()` for GDMA

2. **PPA Operation Modes**:
   - `PPA_TRANS_MODE_BLOCKING`: Blocks until operation completes (simpler, current approach)
   - `PPA_TRANS_MODE_NON_BLOCKING`: Returns immediately, uses callbacks (more complex, allows pipelining)

3. **Memory Alignment**:
   - DMA operations require aligned buffers (typically 32 bytes)
   - Framebuffers are already aligned

4. **Partial Region Operations**:
   - PPA supports `block_offset_x/y` for partial operations
   - Must ensure correct coordinate calculations for rotated outputs
   - After 270° CCW rotation: row at y becomes column at x = (FB_HEIGHT - 1 - y)

## Testing Strategy

1. **Benchmark Before/After**: Measure CPU usage for each optimization
2. **Incremental Implementation**: Implement one optimization at a time
3. **Functional Testing**: Ensure UI correctness after each change
4. **IRAM Usage Monitoring**: Use `idf.py size-components` to track IRAM usage

## Top 5 Must-Do Optimizations

If you can only do 5 things, do these (in order):

1. **Partial PPA Rotation** (#1) - Massive impact (80-95% reduction)
2. **Replace `fb_fill()` with PPA Fill** (#2) - Highest impact, lowest risk
3. **Use PPA Fill for rectangles** (#3) - High impact, low risk
4. **Move Hot Code to IRAM** (#4) - High impact, but watch FreeRTOS API usage
5. **GDMA for memory operations** (#5) - High impact, medium complexity

**Combined Expected Impact**: 70-85% reduction in UI rendering CPU usage

## References

- ESP-IDF PPA Driver: `esp-idf/components/esp_driver_ppa/include/driver/ppa.h`
- ESP-IDF GDMA: `esp-idf/components/esp_hw_support/include/esp_async_memcpy.h`
- ESP-IDF LCD Panel: `esp-idf/components/esp_lcd/include/esp_lcd_panel_ops.h`
