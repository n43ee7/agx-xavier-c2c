import socket, sys, time
mode = sys.argv[1]
if mode == 'srv':
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('10.77.0.2', 10999)); s.listen(1)
    c, _ = s.accept(); c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    while True:
        d = c.recv(64)
        if not d: break
        c.sendall(d)
else:
    c = socket.create_connection(('10.77.0.2', 10999)); c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for n in (16,):
        t = time.time(); N = 2000
        for i in range(N):
            c.sendall(b'x' * n); c.recv(64)
        print(f'TCP request/response {n} B: {(time.time() - t) / N * 1e6:.0f} us')
