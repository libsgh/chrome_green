"""Generate favicon.ico from scratch using pure Python + ctypes (no Pillow)."""
import struct
import math
import os

def create_ico_file(output_path, sizes=[16, 32, 48]):
    """Create a multi-size ICO file with the ChromeGreen logo."""
    
    def draw_logo(size):
        """Draw the ChromeGreen logo as RGBA pixel data."""
        pixels = [[(0, 0, 0, 0)] * size for _ in range(size)]  # RGBA
        
        cx, cy = size / 2, size / 2
        corner_radius = size * 0.19
        
        # Green background with rounded corners
        bg_color = (22, 163, 74, 255)  # #16a34a
        
        for y in range(size):
            for x in range(size):
                # Rounded rectangle check
                in_rect = True
                # Check corners
                for cdx, cdy in [(corner_radius, corner_radius), 
                                  (size - corner_radius, corner_radius),
                                  (corner_radius, size - corner_radius),
                                  (size - corner_radius, size - corner_radius)]:
                    if x < corner_radius and y < corner_radius:
                        dist = math.sqrt((x - cdx)**2 + (y - cdy)**2)
                        if dist > corner_radius:
                            in_rect = False
                    elif x >= size - corner_radius and y < corner_radius:
                        dist = math.sqrt((x - (size - corner_radius))**2 + (y - cdy)**2)
                        if dist > corner_radius:
                            in_rect = False
                    elif x < corner_radius and y >= size - corner_radius:
                        dist = math.sqrt((x - cdx)**2 + (y - (size - corner_radius))**2)
                        if dist > corner_radius:
                            in_rect = False
                    elif x >= size - corner_radius and y >= size - corner_radius:
                        dist = math.sqrt((x - (size - corner_radius))**2 + (y - (size - corner_radius))**2)
                        if dist > corner_radius:
                            in_rect = False
                
                if in_rect:
                    pixels[y][x] = bg_color
        
        # Draw concentric circles (white stroke)
        for radius_frac, stroke_frac in [(0.625, 0.09), (0.3125, 0.078), (0.125, 0.0)]:
            r = size * radius_frac
            if radius_frac == 0.125:
                # Solid filled circle
                for y in range(size):
                    for x in range(size):
                        dist = math.sqrt((x - cx)**2 + (y - cy)**2)
                        if dist <= r:
                            pixels[y][x] = (255, 255, 255, 255)
            else:
                # Ring stroke
                stroke = size * stroke_frac
                for y in range(size):
                    for x in range(size):
                        dist = math.sqrt((x - cx)**2 + (y - cy)**2)
                        if abs(dist - r) <= stroke / 2:
                            pixels[y][x] = (255, 255, 255, 255)
        
        return pixels
    
    # ICO header
    ico_data = struct.pack('<HHH', 0, 1, len(sizes))  # Reserved, Type=1 (ICO), Count
    
    # Calculate offsets
    image_data_offset = 6 + len(sizes) * 16  # header + dir entries
    
    all_images = []
    dir_entries = []
    
    for size in sizes:
        pixels = draw_logo(size)
        
        # Convert RGBA to BGRA for BMP format
        bgra_data = bytearray()
        # BMP stores rows bottom-up
        for y in range(size - 1, -1, -1):
            for x in range(size):
                r, g, b, a = pixels[y][x]
                bgra_data.extend([b, g, r, a])
        
        # Create BMP info header for this image
        # BITMAPINFOHEADER (40 bytes) + pixel data
        bmp_header = struct.pack('<IiiHHIIiiII',
            40,           # biSize
            size,         # biWidth
            size * 2,     # biHeight (2x for XOR + AND masks, bottom-up)
            1,            # biPlanes
            32,           # biBitCount (RGBA)
            0,            # biCompression (BI_RGB)
            len(bgra_data),  # biSizeImage
            0,            # biXPelsPerMeter
            0,            # biYPelsPerMeter
            0,            # biClrUsed
            0,            # biClrImportant
        )
        
        image_data = bmp_header + bytes(bgra_data)
        all_images.append(image_data)
        
        # AND mask (1-bit, all zeros = opaque)
        and_mask_size = ((size + 31) // 32) * 4 * size
        and_mask = bytes(and_mask_size)
        image_data += and_mask
        
        # Directory entry
        dir_entry = struct.pack('<BBBBHHII',
            size if size < 256 else 0,  # width (0 = 256)
            size if size < 256 else 0,  # height (0 = 256)
            0,                          # color count
            0,                          # reserved
            1,                          # color planes
            32,                         # bits per pixel
            len(image_data),            # size of image data
            image_data_offset,          # offset
        )
        dir_entries.append(dir_entry)
        image_data_offset += len(image_data)
    
    ico_data += b''.join(dir_entries)
    for img in all_images:
        ico_data += img
    
    with open(output_path, 'wb') as f:
        f.write(ico_data)

if __name__ == '__main__':
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'public', 'favicon.ico')
    create_ico_file(out)
    print(f"Created {out}")
