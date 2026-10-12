start:
    NOP
    MOV EAX, 100
    MOV EBX, 0
    MOV ECX, 10
    MOV EDX, 20
    MOV ESI, 1000
    MOV EDI, 2000
    MOV EBP, 0
    MOV ESP, 4000

main_loop:
    CMP EAX, 0
    JE program_exit

    CALL process_data
    CALL memory_ops
    CALL stack_manipulation

    DEC EAX
    JMP main_loop

process_data:
    PUSH EAX
    PUSH EBX

    MOV EAX, ECX
    ADD EAX, EDX
    SUB EAX, EBX
    INC EAX
    INC EAX
    DEC EBX
    ADD ECX, 5
    SUB EDX, 2

    CMP ECX, 50
    JE reset_ecx
    JNE continue_process

reset_ecx:
    MOV ECX, 10
    ADD EBX, 1

continue_process:
    CMP EDX, 0
    JE reset_edx
    JNE finish_process

reset_edx:
    MOV EDX, 20
    SUB EBX, 1

finish_process:
    POP EBX
    POP EAX
    RET

memory_ops:
    PUSH EAX
    PUSH ESI
    PUSH EDI

    MOV EAX, 5

memory_loop:
    CMP EAX, 0
    JE memory_done

    LOAD EBX, ESI
    ADD EBX, 10
    STORE EDI, EBX

    INC ESI
    INC EDI
    DEC EAX
    JMP memory_loop

memory_done:
    POP EDI
    POP ESI
    POP EAX
    RET

stack_manipulation:
    PUSH EAX
    PUSH EBX
    PUSH ECX
    PUSH EDX
    
    MOV EAX, 100
    MOV EBX, 200
    MOV ECX, 300
    MOV EDX, 400
    
    PUSH EAX
    PUSH EBX
    PUSH ECX
    PUSH EDX
    
    POP EAX
    POP EBX
    POP ECX
    POP EDX
    
    ADD EAX, EBX
    SUB ECX, EDX
    
    POP EDX
    POP ECX
    POP EBX
    POP EAX
    RET

data_transformation:
    PUSH ESI
    PUSH EDI

    MOV ESI, EBX
    MOV EDI, ECX

    ADD ESI, EDI
    SUB EDI, 5
    INC ESI
    DEC EDI

    CMP ESI, EDI
    JE equal_transform
    JNE unequal_transform

equal_transform:
    ADD EBX, 10
    SUB ECX, 10
    JMP transform_done

unequal_transform:
    SUB EBX, 5
    ADD ECX, 5
    JMP transform_done

transform_done:
    POP EDI
    POP ESI
    RET

extended_math:
    PUSH EAX
    PUSH EDX

    MOV EAX, 1000
    MOV EDX, 500

    ADD EAX, EDX
    SUB EAX, 100
    INC EAX
    INC EDX
    DEC EAX
    DEC EDX
    
    CMP EAX, EDX
    JE skip_math
    ADD EBX, 1

skip_math:
    POP EDX
    POP EAX
    RET

program_exit:
    CALL data_transformation
    CALL extended_math
    
    NOP
    NOP
    NOP
    NOP

halt_loop:
    JMP halt_loop