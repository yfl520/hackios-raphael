"""Query the original lockdown server through the experimental USB/kernel path.
No Pair request, trust override, driver change, reboot, or partition operation.
"""
from pathlib import Path
import plistlib, struct, subprocess, tempfile, time

W = Path(__file__).resolve().parent
FASTBOOT = W / 'tools/platform-tools/fastboot.exe'
MAX_DATA = 16384


class NativeUSBLink:
    def __init__(self):
        self.stream = None
        self.directory = tempfile.TemporaryDirectory(prefix='hakios-link-')
        self.work = Path(self.directory.name)
        self.sequence = 0

    def run(self, *arguments):
        process = subprocess.run([str(FASTBOOT), '-s', 'xnuloader', *map(str, arguments)],
                                 capture_output=True, timeout=5)
        if process.returncode:
            raise RuntimeError((process.stdout + process.stderr).decode('utf-8', 'replace')[-500:])

    def response(self, payload=False):
        destination = self.work / 'response.bin'
        self.run('get_staged', destination)
        response = destination.read_bytes()
        if len(response) < 128:
            raise ValueError('Short native transport header')
        sequence, error, stream, length, connected = struct.unpack_from('<QIIII', response)
        if (not sequence or sequence <= self.sequence or length > MAX_DATA or
                len(response) != 128 + (length if payload else 0) or connected > 1):
            raise ValueError('Invalid or stale native transport response')
        self.sequence = sequence
        return error, stream, length, connected, response[128:]

    def complete(self, command, payload=False):
        try:
            self.run(*command)
        except RuntimeError as error:
            if 'computer link pending' not in str(error):
                raise
            # Never repeat an operation that might already have transferred bytes.
            deadline = time.monotonic() + 3
            while True:
                try:
                    self.run('oem', 'link-status')
                    break
                except RuntimeError as status_error:
                    if 'computer link pending' not in str(status_error) or time.monotonic() >= deadline:
                        raise
                    time.sleep(.05)
        return self.response(payload)

    def open(self):
        error, stream, _, connected, _ = self.complete(('oem', 'link-open', 62078))
        if error:
            raise OSError(error, 'Native kernel connect failed')
        self.stream = stream
        deadline = time.monotonic() + 4
        while not connected:
            error, _, _, connected, _ = self.complete(('oem', 'link-poll', stream))
            if error:
                raise OSError(error, 'Native kernel connect failed')
            if time.monotonic() >= deadline:
                raise TimeoutError('Native kernel connect did not complete')
            if not connected:
                time.sleep(.02)

    def send(self, data):
        pending = memoryview(data)
        deadline = time.monotonic() + 5
        while pending:
            part = bytes(pending[:MAX_DATA])
            source = self.work / 'request.bin'
            source.write_bytes(part)
            self.run('oem', 'link-send', self.stream)
            error, stream, length, _, _ = self.complete(('stage', source))
            if stream != self.stream or length > len(part):
                raise ValueError('Invalid native send acknowledgement')
            if error and error != 35:
                raise OSError(error, 'Native socket send failed')
            if length:
                pending = pending[length:]
            elif time.monotonic() >= deadline:
                raise TimeoutError('Native socket send stalled')
            else:
                time.sleep(.02)

    def receive(self, count, timeout=5):
        output = bytearray()
        deadline = time.monotonic() + timeout
        while len(output) < count:
            error, stream, _, _, data = self.complete(('oem', 'link-recv', self.stream), payload=True)
            if stream != self.stream:
                raise ValueError('Incorrect native receive stream')
            if error and error != 35:
                raise OSError(error, 'Native socket receive failed')
            if data:
                output.extend(data)
            elif not error:
                raise EOFError('Original lockdown server closed the connection')
            elif time.monotonic() >= deadline:
                raise TimeoutError('Original lockdown server did not reply')
            else:
                time.sleep(.02)
        # Read framing must retain excess bytes for the next read.
        return bytes(output)

    def close(self):
        try:
            if self.stream is not None:
                self.complete(('oem', 'link-close', self.stream))
        finally:
            self.stream = None
            self.directory.cleanup()


def query(link, request):
    xml = plistlib.dumps({'Label': 'HackiOSReadOnlyProbe', **request})
    link.send(struct.pack('>I', len(xml)) + xml)
    framed = link.receive(4)
    length = struct.unpack_from('>I', framed)[0]
    if not 0 < length < 1024 * 1024:
        raise ValueError('Invalid native plist response size')
    content = framed[4:]
    if len(content) < length:
        content += link.receive(length - len(content))
    if len(content) != length:
        raise ValueError('Unexpected extra native protocol bytes')
    response = plistlib.loads(content)
    if not isinstance(response, dict):
        raise ValueError('Invalid native plist response type')
    return response

