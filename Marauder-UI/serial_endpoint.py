"""An endpoint owner must not pulse ESP32 reset/download control lines on open."""
import fcntl
import termios
import serial

class EndpointSerial(serial.Serial):
    # pyserial normally applies DTR and RTS with two separate ioctls at open.
    # Their transient combination resets native ESP32 USB-Serial/JTAG on macOS.
    # Dashboard never owns board-reset pins: leave both line states untouched.
    def _update_dtr_state(self): pass
    def _update_rts_state(self): pass

    def open(self):
        super().open()
        try:
            fcntl.ioctl(self.fileno(), termios.TIOCEXCL)
        except Exception:
            super().close()
            raise
