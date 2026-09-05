import math
import threading
import queue
import sys

import pygame
import serial


# ----------------------------
# Configuration
# ----------------------------
PORT = "COM4"
BAUDRATE = 115200          # change if your ESP32 uses a different baudrate
SERIAL_TIMEOUT = 0.2

WINDOW_W, WINDOW_H = 800, 800
CENTER = (WINDOW_W // 2, WINDOW_H // 2)

DIST_MIN, DIST_MAX = -100, 100
MAX_ABS_DIST = 50.0

# Scale distance units to pixels (250 units -> ~350 px radius)
RADIUS_PIXELS = 350
SCALE = RADIUS_PIXELS / MAX_ABS_DIST

DOT_COLOR = (0, 255, 0)
DOT_RADIUS = 2

BG_COLOR = (0, 0, 0)
AXIS_COLOR = (60, 60, 60)


# ----------------------------
# Serial reader thread
# ----------------------------
def serial_reader(port, baudrate, out_queue, stop_event):
    try:
        ser = serial.Serial(port, baudrate, timeout=SERIAL_TIMEOUT)
    except Exception as e:
        out_queue.put(("__ERROR__", f"Could not open {port}: {e}"))
        return

    with ser:
        while not stop_event.is_set():
            try:
                line = ser.readline().decode(errors="ignore").strip()
                if not line:
                    continue

                # Expect: "distance,alpha"
                parts = line.split(",")
                if len(parts) != 2:
                    continue

                distance = float(parts[0].strip())
                alpha_deg = float(parts[1].strip())

                # Optional: clamp distance to expected range
                if distance < DIST_MIN:
                    distance = DIST_MIN
                elif distance > DIST_MAX:
                    distance = DIST_MAX

                # Normalize angle (0..360)
                alpha_deg = alpha_deg % 360.0

                out_queue.put((distance, alpha_deg))

            except Exception:
                # Ignore malformed lines / transient errors
                continue


# ----------------------------
# Main pygame app
# ----------------------------
def main():
    pygame.init()
    screen = pygame.display.set_mode((WINDOW_W, WINDOW_H))
    pygame.display.set_caption("ESP32 Serial Trace (distance, alpha)")
    clock = pygame.time.Clock()

    # We'll draw onto a persistent surface so old dots remain
    canvas = pygame.Surface((WINDOW_W, WINDOW_H))
    canvas.fill(BG_COLOR)

    # Draw axes once
    pygame.draw.line(canvas, AXIS_COLOR, (0, CENTER[1]), (WINDOW_W, CENTER[1]), 1)
    pygame.draw.line(canvas, AXIS_COLOR, (CENTER[0], 0), (CENTER[0], WINDOW_H), 1)
    pygame.draw.circle(canvas, AXIS_COLOR, CENTER, RADIUS_PIXELS, 1)

    data_queue = queue.Queue()
    stop_event = threading.Event()

    t = threading.Thread(
        target=serial_reader,
        args=(PORT, BAUDRATE, data_queue, stop_event),
        daemon=True
    )
    t.start()

    running = True
    font = pygame.font.SysFont(None, 22)
    last_status = f"Reading {PORT} @ {BAUDRATE}..."

    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

            # Optional: press C to clear trace
            if event.type == pygame.KEYDOWN and event.key == pygame.K_c:
                canvas.fill(BG_COLOR)
                pygame.draw.line(canvas, AXIS_COLOR, (0, CENTER[1]), (WINDOW_W, CENTER[1]), 1)
                pygame.draw.line(canvas, AXIS_COLOR, (CENTER[0], 0), (CENTER[0], WINDOW_H), 1)
                pygame.draw.circle(canvas, AXIS_COLOR, CENTER, RADIUS_PIXELS, 1)

        # Consume all queued serial samples this frame
        while True:
            try:
                item = data_queue.get_nowait()
            except queue.Empty:
                break

            if item[0] == "__ERROR__":
                last_status = f"ERROR: {item[1]}"
                continue

            distance, alpha_deg = item
            alpha_rad = math.radians(alpha_deg)

            # Convert to x,y (math coords: +y up). Pygame y is down, so invert y.
            x = distance * math.cos(alpha_rad)
            y = distance * math.sin(alpha_rad)

            px = int(CENTER[0] - x * SCALE)
            py = int(CENTER[1] + y * SCALE)

            # Draw dot onto persistent canvas
            pygame.draw.circle(canvas, DOT_COLOR, (px, py), DOT_RADIUS)

            last_status = f"distance={distance:.2f}, alpha={alpha_deg:.2f} -> x={x:.2f}, y={y:.2f}"

        # Blit persistent canvas
        screen.blit(canvas, (0, 0))

        # Status text overlay
        text = font.render(last_status + "   (Press C to clear)", True, (200, 200, 200))
        screen.blit(text, (10, 10))

        pygame.display.flip()
        clock.tick(60)

    stop_event.set()
    pygame.quit()
    sys.exit(0)


if __name__ == "__main__":
    main()