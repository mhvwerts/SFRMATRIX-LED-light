#!/usr/bin/env python3
"""
NiceGUI front-end for the Arduino LED PWM firmware `led_pwm_serial_w_button_ctrl.ino`

Run: `python LED_control_gui.py`     (opens http://localhost:8080)

Serial protocol (115200 baud, one line per command):
    SET <0..255>  ->  "OK <level>"  or  "ERR <reason>"
    GET           ->  "OK <level>"

The level can also be changed on the device itself with its momentary
switch (the sketch stays silent when that happens). To keep the slider in
step, the app polls the device with GET every POLL_INTERVAL seconds and
moves the slider to the reported level.
"""
import threading
import time

import serial
from serial.tools import list_ports
from nicegui import app, run, ui

BAUD = 115200
SEND_INTERVAL = 0.05   # s, slider changes are coalesced to at most 20 commands/s
POLL_INTERVAL = 1.0    # s, how often the device level is read back
USER_HOLDOFF = 1.0     # s, no polling for this long after the user touched the slider


class LedLink:
    """Thread-safe wrapper around the serial port."""

    def __init__(self):
        self.ser = None
        self.lock = threading.Lock()

    @property
    def connected(self) -> bool:
        return self.ser is not None and self.ser.is_open

    def connect(self, port: str) -> int:
        """Open the port, wait for the Arduino to reboot, return current level."""
        ser = serial.Serial(port, BAUD, timeout=1)
        try:
            # Opening the port resets most Arduinos; wait for the READY banner.
            deadline = time.time() + 4
            while time.time() < deadline:
                line = ser.readline().decode(errors='replace').strip()
                if line.startswith('READY'):
                    break
            time.sleep(0.3)
            ser.reset_input_buffer()
            self.ser = ser
            return self.command('GET')
        except Exception:
            self.close()
            ser.close()
            raise

    def command(self, cmd: str) -> int:
        """Send a command, return the level from the 'OK <level>' reply."""
        with self.lock:
            if not self.connected:
                raise RuntimeError('not connected')
            self.ser.write((cmd + '\n').encode())
            reply = self.ser.readline().decode(errors='replace').strip()
        if reply.startswith('OK '):
            return int(reply[3:])
        raise RuntimeError(reply or 'no reply from Arduino (timeout)')

    def set_level(self, value: int) -> int:
        return self.command(f'SET {int(value)}')

    def get_level(self) -> int:
        return self.command('GET')

    def close(self):
        with self.lock:
            if self.ser is not None:
                try:
                    self.ser.close()
                except Exception:
                    pass
            self.ser = None


link = LedLink()
state = {
    'pending': None,        # slider value waiting to be sent
    'last_sent': None,      # level the device is known to have
    'busy': False,          # a serial transaction is in flight (send or poll)
    'from_device': False,   # True while we move the slider ourselves (poll result)
    'seq': 0,               # counts user slider changes, to detect races with a poll
    'last_user': 0.0,       # time.monotonic() of the last user slider change
}


def port_options() -> dict:
    return {p.device: f'{p.device}  {p.description}' for p in sorted(list_ports.comports())}


@ui.page('/')
def index():
    ui.page_title('LED PWM Control')

    with ui.card().classes('w-96 mx-auto mt-8 gap-3'):
        ui.label('LED PWM Control').classes('text-xl font-bold')

        # ---- connection row -------------------------------------------------
        with ui.row().classes('w-full items-center no-wrap'):
            opts = port_options()
            port_select = ui.select(opts, value=next(iter(opts), None), label='Serial port') \
                .classes('grow')
            refresh_btn = ui.button(icon='refresh').props('flat round dense')
        connect_btn = ui.button('Connect').classes('w-full')
        status = ui.label('Disconnected').classes('text-sm text-gray-500')

        ui.separator()

        # ---- control --------------------------------------------------------
        slider = ui.slider(min=0, max=255, step=1, value=50).props('label-always')
        readout = ui.label().classes('text-lg self-center')
        readout.bind_text_from(
            slider, 'value', lambda v: f'{int(v)} / 255   ({v / 255 * 100:.1f} %)')

        with ui.row().classes('w-full justify-between'):
            presets = [
                ui.button('Off', on_click=lambda: slider.set_value(0)),
                ui.button('20 %', on_click=lambda: slider.set_value(51)),
                ui.button('50 %', on_click=lambda: slider.set_value(128)),
                ui.button('Max', on_click=lambda: slider.set_value(255)),
            ]
        for b in presets:
            b.props('outline dense')

    # ---- helpers ------------------------------------------------------------
    def update_ui():
        on = link.connected
        slider.set_enabled(on)
        for b in presets:
            b.set_enabled(on)
        port_select.set_enabled(not on)
        refresh_btn.set_enabled(not on)
        connect_btn.set_text('Disconnect' if on else 'Connect')
        connect_btn.set_enabled(True)

    def handle_error(e: Exception):
        link.close()
        state['pending'] = None
        status.set_text(f'Error: {e}')
        ui.notify(f'Connection lost: {e}', type='negative')
        update_ui()

    def refresh_ports():
        options = port_options()
        port_select.set_options(options, value=port_select.value if port_select.value in options
                                else next(iter(options), None))

    refresh_btn.on_click(refresh_ports)

    def move_slider_from_device(level: int):
        """Move the slider without it counting as a user change (no echo SET)."""
        state['from_device'] = True
        try:
            slider.set_value(level)
        finally:
            state['from_device'] = False

    async def toggle_connection():
        if link.connected:
            link.close()
            status.set_text('Disconnected')
            update_ui()
            return
        if not port_select.value:
            ui.notify('No serial port selected', type='warning')
            return
        connect_btn.set_enabled(False)
        status.set_text('Connecting (waiting for Arduino reset)...')
        try:
            level = await run.io_bound(link.connect, port_select.value)
        except Exception as e:
            status.set_text(f'Connection failed: {e}')
            ui.notify(f'Connection failed: {e}', type='negative')
            update_ui()
            return
        state['last_sent'] = level          # adopt the board's level, no echo
        state['pending'] = None
        move_slider_from_device(level)
        status.set_text(f'Connected to {port_select.value}')
        update_ui()

    connect_btn.on_click(toggle_connection)

    def on_slider_change(e):
        if state['from_device']:            # we moved it ourselves, not the user
            return
        state['seq'] += 1
        state['last_user'] = time.monotonic()
        if link.connected and e.value is not None:
            state['pending'] = int(e.value)

    slider.on_value_change(on_slider_change)

    async def flush():
        """Send only the latest slider value, at most once per SEND_INTERVAL."""
        value = state['pending']
        if value is None or state['busy'] or not link.connected:
            return
        state['pending'] = None
        if value == state['last_sent']:
            return
        state['busy'] = True
        try:
            state['last_sent'] = await run.io_bound(link.set_level, value)
        except Exception as e:
            handle_error(e)
        finally:
            state['busy'] = False

    async def poll():
        """Read the device level back, so changes made with its switch show up.

        The reply is thrown away if the user touched the slider while the
        request was in flight; otherwise a stale value could jump the slider
        back under their finger.
        """
        if not link.connected or state['busy'] or state['pending'] is not None:
            return
        if time.monotonic() - state['last_user'] < USER_HOLDOFF:
            return
        seq = state['seq']
        state['busy'] = True
        try:
            level = await run.io_bound(link.get_level)
        except Exception as e:
            handle_error(e)
            return
        finally:
            state['busy'] = False
        if seq != state['seq'] or state['pending'] is not None:
            return                          # user moved the slider meanwhile
        state['last_sent'] = level          # the device's level, so no echo SET
        if level != int(slider.value):
            move_slider_from_device(level)

    ui.timer(SEND_INTERVAL, flush)
    ui.timer(POLL_INTERVAL, poll)
    update_ui()


app.on_shutdown(link.close)

if __name__ in {'__main__', '__mp_main__'}:
    # hoststr - Address the web interface listens on:
    #   '127.0.0.1' -> reachable only from this machine (safe default)
    #   None        -> NiceGUI's 'unsafe' default (0.0.0.0): reachable 
    #                  from the local network, with no access control
    hoststr = '127.0.0.1'
    
    ui.run(title='LED PWM Control', host=hoststr, port=8080, reload=False)
