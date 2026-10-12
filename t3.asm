start:
    mov ax, 0
    mov cx, 5
loop:
    add ax, cx
    sub cx, 1
    cmp cx, 0
    jne loop
    ret