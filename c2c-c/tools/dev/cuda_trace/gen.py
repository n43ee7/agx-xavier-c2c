names=[l.strip() for l in open('syms.txt') if l.strip()]
names=[n for n in names if n not in ('cuGetProcAddress','cuGetExportTable','cuCtxSetCurrent','cuCtxGetCurrent','cuDevicePrimaryCtxRetain','cuCtxGetDevice')]
with open('tramp.S','w') as f:
    f.write('.text\n')
    for i,n in enumerate(names):
        f.write(f'''.global {n}
.type {n}, %function
{n}:
  stp x29, x30, [sp, -96]!
  stp x0, x1, [sp, 16]
  stp x2, x3, [sp, 32]
  stp x4, x5, [sp, 48]
  stp x6, x7, [sp, 64]
  str x8, [sp, 80]
  mov x0, {i}
  bl trace_hit
  mov x16, x0
  ldr x8, [sp, 80]
  ldp x6, x7, [sp, 64]
  ldp x4, x5, [sp, 48]
  ldp x2, x3, [sp, 32]
  ldp x0, x1, [sp, 16]
  ldp x29, x30, [sp], 96
  br x16
''')
with open('names.h','w') as f:
    f.write('static const char* kNames[] = {\n'+''.join(f'"{n}",\n' for n in names)+'};\n')
with open('tramp.S','a') as f:
    f.write('.section .data.rel.ro\n.global tramp_table\ntramp_table:\n'+''.join(f'  .quad {n}\n' for n in names))
T,SL=24,96
with open('etramp.S','w') as f:
    f.write('.text\n')
    for t in range(T):
        for s in range(SL):
            f.write(f'''et_{t}_{s}:
  stp x29, x30, [sp, -96]!
  stp x0, x1, [sp, 16]
  stp x2, x3, [sp, 32]
  stp x4, x5, [sp, 48]
  stp x6, x7, [sp, 64]
  str x8, [sp, 80]
  mov x0, {t}
  mov x1, {s}
  bl et_hit
  mov x16, x0
  ldr x8, [sp, 80]
  ldp x6, x7, [sp, 64]
  ldp x4, x5, [sp, 48]
  ldp x2, x3, [sp, 32]
  ldp x0, x1, [sp, 16]
  blr x16
  mov x1, x0
  mov x0, {t*96+s}
  add x2, sp, 16
  bl et_ret
  ldp x29, x30, [sp], 96
  ret
''')
    f.write('.section .data.rel.ro\n.global et_tramps\net_tramps:\n')
    for t in range(T):
        for s in range(SL): f.write(f'  .quad et_{t}_{s}\n')
