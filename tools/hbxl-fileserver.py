#!/usr/bin/env python3
"""File and console server for Linux-personality apps (sdk/linux/vfs.c).

Serves one directory tree to the console over TCP (default 10.42.0.1:39501).
Paths are confined to the root.  stdout/stderr of the app are printed here.

  request  u32 magic 'HBFS', u32 op, u32 length, u32 seq, payload
  reply    u32 magic 'HBFS', i32 status, u32 length, u32 seq, payload
"""
import argparse
import errno
import os
import socket
import stat
import struct
import sys
import threading
import time

MAGIC = 0x53464248
VERBOSE = os.environ.get("HBXL_FS_VERBOSE") == "1"
OP_HELLO, OP_OPEN, OP_CLOSE, OP_PREAD, OP_PWRITE, OP_STAT, OP_FSTAT, OP_LISTDIR, OP_LOG, \
    OP_MKDIR, OP_UNLINK, OP_RENAME, OP_TRUNCATE = range(1, 14)


def recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        part = conn.recv(min(n - len(buf), 1 << 20))
        if not part:
            raise ConnectionError("closed")
        buf += part
    return bytes(buf)


class Session:
    def __init__(self, conn, root, log):
        self.conn, self.root, self.log = conn, os.path.realpath(root), log
        self.handles, self.next = {}, 1

    def resolve(self, path):
        path = path.split(b"\0", 1)[0].decode("utf-8", "surrogateescape")
        full = os.path.realpath(os.path.join(self.root, path.lstrip("/")))
        if full != self.root and not full.startswith(self.root + os.sep):
            raise PermissionError(errno.EACCES, "outside root")
        return full

    def reply(self, seq, status, payload=b""):
        self.conn.sendall(struct.pack("<IiII", MAGIC, status, len(payload), seq) + payload)

    def stat_payload(self, st):
        return struct.pack("<QIIq", st.st_size, st.st_mode, 0, int(st.st_mtime))

    def handle(self, op, seq, data):
        if op == OP_LOG:
            fd, = struct.unpack_from("<I", data)
            text = data[4:].decode("utf-8", "replace")
            self.log.write(text)
            self.log.flush()
            return
        try:
            if op == OP_HELLO:
                return self.reply(seq, 0, struct.pack("<q", time.time_ns()))
            if op == OP_OPEN:
                flags, mode = struct.unpack_from("<II", data)
                path = self.resolve(data[8:])
                if VERBOSE:
                    print(f"[fs] open {data[8:].split(bytes(1))[0].decode(errors='replace')} "
                          f"flags=0x{flags:x} exists={os.path.exists(path)}", flush=True)
                if os.path.isdir(path):
                    h = self.next; self.next += 1
                    self.handles[h] = ("dir", path)
                    return self.reply(seq, h | (1 << 30))
                acc = flags & 3
                pyflags = {0: os.O_RDONLY, 1: os.O_WRONLY, 2: os.O_RDWR}[acc]
                if flags & 0o100: pyflags |= os.O_CREAT
                if flags & 0o1000: pyflags |= os.O_TRUNC
                if flags & 0o200: pyflags |= os.O_EXCL
                if flags & 0o2000: pyflags |= os.O_APPEND
                if pyflags & os.O_CREAT:
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                fd = os.open(path, pyflags, mode or 0o644)
                h = self.next; self.next += 1
                self.handles[h] = ("file", fd)
                return self.reply(seq, h)
            if op == OP_CLOSE:
                h, = struct.unpack_from("<I", data)
                kind, obj = self.handles.pop(h, (None, None))
                if kind == "file":
                    os.close(obj)
                return self.reply(seq, 0)
            if op == OP_PREAD:
                h, n, off = struct.unpack_from("<IIQ", data)
                kind, fd = self.handles[h]
                chunk = os.pread(fd, n, off)
                return self.reply(seq, len(chunk), chunk)
            if op == OP_PWRITE:
                h, append, off = struct.unpack_from("<IIQ", data)
                kind, fd = self.handles[h]
                body = data[16:]
                n = os.write(fd, body) if append else os.pwrite(fd, body, off)
                return self.reply(seq, n)
            if op == OP_STAT:
                if VERBOSE:
                    print(f"[fs] stat {data.split(bytes(1))[0].decode(errors='replace')}", flush=True)
                return self.reply(seq, 0, self.stat_payload(os.stat(self.resolve(data))))
            if op == OP_FSTAT:
                h, = struct.unpack_from("<I", data)
                kind, obj = self.handles[h]
                st = os.fstat(obj) if kind == "file" else os.stat(obj)
                return self.reply(seq, 0, self.stat_payload(st))
            if op == OP_LISTDIR:
                h, = struct.unpack_from("<I", data)
                kind, path = self.handles[h]
                out = bytearray()
                names = [".", ".."] + sorted(os.listdir(path))
                for name in names:
                    full = os.path.join(path, name)
                    t = 4 if os.path.isdir(full) else 8
                    raw = name.encode("utf-8", "surrogateescape")
                    out += struct.pack("<BH", t, len(raw)) + raw
                return self.reply(seq, len(names), bytes(out))
            if op == OP_MKDIR:
                os.mkdir(self.resolve(data))
                return self.reply(seq, 0)
            if op == OP_UNLINK:
                path = self.resolve(data)
                (os.rmdir if os.path.isdir(path) else os.unlink)(path)
                return self.reply(seq, 0)
            if op == OP_RENAME:
                a, b = data.split(b"\0")[:2]
                os.replace(self.resolve(a), self.resolve(b))
                return self.reply(seq, 0)
            if op == OP_TRUNCATE:
                h, size = struct.unpack_from("<IQ", data)
                os.ftruncate(self.handles[h][1], size)
                return self.reply(seq, 0)
            return self.reply(seq, -errno.ENOSYS)
        except OSError as e:
            return self.reply(seq, -(e.errno or errno.EIO))
        except (KeyError, struct.error, ValueError):
            return self.reply(seq, -errno.EBADF)

    def run(self):
        while True:
            magic, op, length, seq = struct.unpack("<IIII", recv_exact(self.conn, 16))
            if magic != MAGIC:
                raise ConnectionError("bad magic")
            self.handle(op, seq, recv_exact(self.conn, length) if length else b"")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--host", default="10.42.0.1")
    ap.add_argument("--port", type=int, default=39501)
    args = ap.parse_args()
    os.makedirs(args.root, exist_ok=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(4)
    print(f"hbxl-fileserver: serving {os.path.realpath(args.root)} on {args.host}:{args.port}", flush=True)
    while True:
        conn, peer = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"hbxl-fileserver: client {peer[0]}:{peer[1]}", flush=True)

        def serve(c=conn):
            try:
                Session(c, args.root, sys.stdout).run()
            except (ConnectionError, OSError) as e:
                print(f"\nhbxl-fileserver: client gone ({e})", flush=True)
            finally:
                c.close()
        threading.Thread(target=serve, daemon=True).start()


if __name__ == "__main__":
    main()
