import argparse
import datetime
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture a short serial log to logs/serial_YYYYMMDD_HHMMSS.log")
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--reset", action="store_true", help="Toggle DTR/RTS before capture")
    args = parser.parse_args()

    log_path = "logs/serial_%s.log" % datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    print("Capturing serial to", log_path)

    start = time.time()
    deadline = start + float(args.seconds)

    with serial.Serial(args.port, baudrate=args.baud, timeout=0.2) as ser, open(
        log_path, "w", encoding="utf-8", errors="replace"
    ) as out:
        if args.reset:
            try:
                ser.dtr = False
                ser.rts = False
                time.sleep(0.05)
                ser.dtr = True
                ser.rts = True
                time.sleep(0.05)
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass

        while time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            text = chunk.decode("utf-8", errors="replace")
            out.write(text)
            out.flush()
            print(text, end="")

    print("\n--- done ---")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
