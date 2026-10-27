; pixel_asm.s - heisse Pixelschleifen in 68k-Assembler (vasm, Motorola-Syntax)
;
; Aufrufkonvention wie gcc: Argumente als Langworte auf dem Stapel,
; d0-d1/a0-a1 duerfen veraendert werden, alles andere wird gesichert.
; Jede Routine liefert bitgenau dasselbe Ergebnis wie ihre C-Referenz in
; pixel.c (_c); "AmiPixelShop BENCH" prueft das.

        xref    _chk_tab_ff
        xref    _chk_tab_cc
        xref    _blur_h_c
        xref    _blur_v_c

        xdef    _span_show_mix_asm
        xdef    _span_show_mix_map_asm
        xdef    _span_layer_over_asm
        xdef    _blur_h_asm
        xdef    _blur_v_asm

        section code,code

;---------------------------------------------------------------------------
; void span_show_mix_asm(ULONG *d, const ULONG *s, LONG n, LONG cx, LONG cyp)
; premultiplied ueber Schachbrett: d = (p + tab[a]) | $ff000000
; Das Schachbrett wechselt alle 8 Pixel: Tabelle je Lauf einmal waehlen.
;---------------------------------------------------------------------------
_span_show_mix_asm:
        movem.l d2-d7/a2,-(sp)          ; 7 Register = 28 Bytes
        movem.l 32(sp),a0/a1            ; d, s
        move.l  40(sp),d7               ; n
        move.l  44(sp),d6               ; cx
        move.l  48(sp),d5               ; cyp
        tst.l   d7
        ble.s   .done
.run:   move.l  d6,d0
        asr.l   #3,d0
        eor.l   d5,d0
        btst    #0,d0
        lea     _chk_tab_ff,a2
        beq.s   .tab
        lea     _chk_tab_cc,a2
.tab:   moveq   #7,d1
        and.l   d6,d1
        moveq   #8,d2
        sub.l   d1,d2                   ; k = 8 - (cx & 7)
        cmp.l   d7,d2
        ble.s   .klen
        move.l  d7,d2
.klen:  add.l   d2,d6                   ; cx += k
        sub.l   d2,d7                   ; n -= k
        subq.l  #1,d2
.px:    move.l  (a1)+,d0
        move.l  d0,d1
        rol.l   #8,d1
        and.l   #$ff,d1
        add.l   (a2,d1.l*4),d0
        or.l    #$ff000000,d0
        move.l  d0,(a0)+
        dbra    d2,.px
        tst.l   d7
        bgt.s   .run
.done:  movem.l (sp)+,d2-d7/a2
        rts

;---------------------------------------------------------------------------
; void span_show_mix_map_asm(ULONG *d, const ULONG *row, const LONG *xmap,
;                            LONG n, LONG cx, LONG cyp)
; wie oben, Quelle ueber Spaltentabelle (Zoom)
;---------------------------------------------------------------------------
_span_show_mix_map_asm:
        movem.l d2-d7/a2-a3,-(sp)       ; 8 Register = 32 Bytes
        movem.l 36(sp),a0/a1/a3         ; d, row, xmap
        move.l  48(sp),d7               ; n
        move.l  52(sp),d6               ; cx
        move.l  56(sp),d5               ; cyp
        tst.l   d7
        ble.s   .done
.run:   move.l  d6,d0
        asr.l   #3,d0
        eor.l   d5,d0
        btst    #0,d0
        lea     _chk_tab_ff,a2
        beq.s   .tab
        lea     _chk_tab_cc,a2
.tab:   moveq   #7,d1
        and.l   d6,d1
        moveq   #8,d2
        sub.l   d1,d2
        cmp.l   d7,d2
        ble.s   .klen
        move.l  d7,d2
.klen:  add.l   d2,d6
        sub.l   d2,d7
        subq.l  #1,d2
.px:    move.l  (a3)+,d1
        move.l  (a1,d1.l*4),d0
        move.l  d0,d1
        rol.l   #8,d1
        and.l   #$ff,d1
        add.l   (a2,d1.l*4),d0
        or.l    #$ff000000,d0
        move.l  d0,(a0)+
        dbra    d2,.px
        tst.l   d7
        bgt.s   .run
.done:  movem.l (sp)+,d2-d7/a2-a3
        rts

;---------------------------------------------------------------------------
; void span_layer_over_asm(ULONG *d, const ULONG *s, LONG n, ULONG op)
; d (premultiplied) = premul(s * op) + d * (1 - sa)
;---------------------------------------------------------------------------
_span_layer_over_asm:
        movem.l d2-d7,-(sp)             ; 6 Register = 24 Bytes
        movem.l 28(sp),a0/a1            ; d, s
        move.l  36(sp),d7               ; n
        move.l  40(sp),d6               ; op
        move.l  d6,d0
        lsr.l   #7,d0
        add.l   d0,d6                   ; op' = op + (op >> 7), 0..256
        tst.l   d7
        ble     .done
.px:    move.l  (a1)+,d0                ; p
        move.l  d0,d1
        rol.l   #8,d1
        and.l   #$ff,d1
        mulu.w  d6,d1
        lsr.l   #8,d1                   ; a = (sA * op') >> 8
        beq.s   .skip                   ; unsichtbar: Ziel bleibt
        cmp.l   #255,d1
        beq.s   .opaque                 ; deckend: Ziel = Quelle
        move.l  d1,d2
        lsr.l   #7,d2
        add.l   d1,d2                   ; a2 = a + (a >> 7)
        move.l  #256,d3
        sub.l   d2,d3                   ; inv = 256 - a2
        move.l  d1,d4
        ror.l   #8,d4                   ; a << 24
        move.l  d0,d5
        and.l   #$ff00ff,d5
        mulu.l  d2,d5
        lsr.l   #8,d5
        and.l   #$ff00ff,d5
        or.l    d5,d4
        and.l   #$ff00,d0
        mulu.l  d2,d0
        lsr.l   #8,d0
        and.l   #$ff00,d0
        or.l    d0,d4                   ; premul(s)
        move.l  (a0),d0                 ; q
        move.l  d0,d5
        and.l   #$ff00ff,d5
        mulu.l  d3,d5
        lsr.l   #8,d5
        and.l   #$ff00ff,d5
        add.l   d5,d4
        lsr.l   #8,d0
        and.l   #$ff00ff,d0
        mulu.l  d3,d0
        and.l   #$ff00ff00,d0
        add.l   d0,d4
        move.l  d4,(a0)+
        subq.l  #1,d7
        bne     .px
        bra.s   .done
.skip:  addq.l  #4,a0
        subq.l  #1,d7
        bne     .px
        bra.s   .done
.opaque:
        move.l  d0,(a0)+
        subq.l  #1,d7
        bne     .px
.done:  movem.l (sp)+,d2-d7
        rts

;---------------------------------------------------------------------------
; void blur_h_asm(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r,
;                 ULONG *pad)
; Box waagerecht. Jede Zeile wird zuerst in pad kopiert und um r mit dem
; Randpixel aufgefuellt - die Fensterschleife ist dann sprungfrei.
; Summen als 16-Bit-Spuren: d4 = R|B, d5 = A|G. Exakt bis r = 128.
;---------------------------------------------------------------------------
_blur_h_asm:
        cmp.l   #128,20(sp)
        ble.s   .ok
        jmp     _blur_h_c               ; gleiche Argumente: einfach springen
.ok:    movem.l d2-d7/a2-a6,-(sp)       ; 11 Register = 44 Bytes
        movem.l 48(sp),a0/a1            ; src, dst
        move.l  64(sp),d0               ; r
        add.l   d0,d0
        addq.l  #1,d0
        move.l  d0,a5                   ; a5 = 2r + 1 (Fensterbreite)
        move.l  #65536,d6
        divu.l  d0,d6                   ; inv = 65536 / (2r + 1)
        move.l  60(sp),d7               ; h
.row:   ; --- pad fuellen: r mal links, w Pixel, r+1 mal rechts ---
        move.l  68(sp),a2               ; pad
        move.l  (a0),d0                 ; erstes Pixel
        move.l  64(sp),d1
        bra.s   .l1
.l0:    move.l  d0,(a2)+
.l1:    dbra    d1,.l0
        move.l  56(sp),d1               ; w
        move.l  a0,a3
        bra.s   .m1
.m0:    move.l  (a3)+,(a2)+
.m1:    dbra    d1,.m0
        move.l  -4(a3),d0               ; letztes Pixel
        move.l  64(sp),d1               ; r + 1 mal
.r0:    move.l  d0,(a2)+
        dbra    d1,.r0
        ; --- Anfangsfenster pad[0 .. 2r] ---
        moveq   #0,d4
        moveq   #0,d5
        move.l  68(sp),a3               ; po = pad
        move.l  a3,a2
        move.l  a5,d1
        subq.l  #1,d1
.i0:    move.l  (a2)+,d0
        move.l  d0,d2
        and.l   #$ff00ff,d2
        add.l   d2,d4
        lsr.l   #8,d0
        and.l   #$ff00ff,d0
        add.l   d0,d5
        dbra    d1,.i0
        move.l  a2,a4                   ; pi = pad + 2r + 1
        ; --- Fenster schieben ---
        move.l  56(sp),d3               ; x-Zaehler = w
.px:    move.l  d5,d0                   ; A
        swap    d0
        mulu.w  d6,d0
        add.l   #$8000,d0
        clr.w   d0
        lsl.l   #8,d0
        move.l  d0,d1
        move.l  d4,d0                   ; R
        swap    d0
        mulu.w  d6,d0
        add.l   #$8000,d0
        clr.w   d0
        or.l    d0,d1
        move.l  d5,d0                   ; G
        mulu.w  d6,d0
        add.l   #$8000,d0
        clr.w   d0
        lsr.l   #8,d0
        or.l    d0,d1
        move.l  d4,d0                   ; B
        mulu.w  d6,d0
        add.l   #$8000,d0
        swap    d0
        and.l   #$ff,d0
        or.l    d0,d1
        move.l  d1,(a1)+
        move.l  (a4)+,d0                ; vi
        move.l  (a3)+,d1                ; vo
        move.l  d0,d2
        and.l   #$ff00ff,d2
        add.l   d2,d4
        move.l  d1,d2
        and.l   #$ff00ff,d2
        sub.l   d2,d4
        lsr.l   #8,d0
        and.l   #$ff00ff,d0
        add.l   d0,d5
        lsr.l   #8,d1
        and.l   #$ff00ff,d1
        sub.l   d1,d5
        subq.l  #1,d3
        bne     .px
        move.l  56(sp),d0               ; naechste Zeile
        lsl.l   #2,d0
        add.l   d0,a0
        subq.l  #1,d7
        bne     .row
        movem.l (sp)+,d2-d7/a2-a6
        rts

;---------------------------------------------------------------------------
; void blur_v_asm(const ULONG *src, ULONG *dst, LONG w, LONG h, LONG r,
;                 ULONG *sums)
; Box senkrecht, zeilenweise mit Spaltensummen (sums: 2*w Langworte,
; erst R|B, dahinter A|G). Exakt bis r = 128, darueber C.
;---------------------------------------------------------------------------
_blur_v_asm:
        cmp.l   #128,20(sp)
        ble.s   .ok
        jmp     _blur_v_c
.ok:
        movem.l d2-d7/a2-a6,-(sp)       ; 44 Bytes -> Argumente ab 48(sp)
        move.l  48(sp),a0               ; src
        move.l  52(sp),a1               ; dst
        move.l  56(sp),d7               ; w
        move.l  68(sp),a2               ; sums
        move.l  64(sp),d0               ; r
        add.l   d0,d0
        addq.l  #1,d0
        move.l  #65536,d6
        divu.l  d0,d6                   ; inv
        move.l  d7,d4
        lsl.l   #2,d4                   ; d4 = w*4: Abstand R|B -> A|G
        move.l  d4,a5                   ; a5 = Zeilenlaenge in Bytes
        ; Summen loeschen
        move.l  a2,a6
        move.l  d7,d0
        add.l   d0,d0
.clr:   clr.l   (a6)+
        subq.l  #1,d0
        bne.s   .clr
        ; Anfangsfenster i = -r .. r
        move.l  64(sp),d5
        neg.l   d5                      ; i
.init:  move.l  d5,d0
        bpl.s   .c1
        moveq   #0,d0
.c1:    cmp.l   60(sp),d0
        blt.s   .c2
        move.l  60(sp),d0
        subq.l  #1,d0
.c2:    move.l  a5,d1
        mulu.l  d0,d1
        lea     (a0,d1.l),a3            ; Quellzeile
        move.l  a2,a6
        move.l  d7,d3
.ia:    move.l  (a3)+,d0
        move.l  d0,d1
        lsr.l   #8,d1
        and.l   #$ff00ff,d1
        add.l   d1,(a6,d4.l)
        and.l   #$ff00ff,d0
        add.l   d0,(a6)+
        subq.l  #1,d3
        bne.s   .ia
        addq.l  #1,d5
        cmp.l   64(sp),d5
        ble.s   .init
        ; Zeilen
        moveq   #0,d5                   ; y
.row:   move.l  d5,d0                   ; so = max(y - r, 0)
        sub.l   64(sp),d0
        bpl.s   .r1
        moveq   #0,d0
.r1:    move.l  a5,d1
        mulu.l  d0,d1
        lea     (a0,d1.l),a3
        move.l  d5,d0                   ; si = min(y + r + 1, h - 1)
        add.l   64(sp),d0
        addq.l  #1,d0
        cmp.l   60(sp),d0
        blt.s   .r2
        move.l  60(sp),d0
        subq.l  #1,d0
.r2:    move.l  a5,d1
        mulu.l  d0,d1
        lea     (a0,d1.l),a4
        move.l  a2,a6
        move.l  d7,d3
.px:    move.l  (a6,d4.l),d2            ; A|G
        move.l  d2,d0
        swap    d0
        mulu.w  d6,d0
        add.l   #$8000,d0
        clr.w   d0
        lsl.l   #8,d0
        move.l  d0,d1                   ; A
        move.l  d2,d0
        mulu.w  d6,d0
        add.l   #$8000,d0
        clr.w   d0
        lsr.l   #8,d0
        or.l    d0,d1                   ; G
        move.l  (a6),d2                 ; R|B
        move.l  d2,d0
        swap    d0
        mulu.w  d6,d0
        add.l   #$8000,d0
        clr.w   d0
        or.l    d0,d1                   ; R
        move.l  d2,d0
        mulu.w  d6,d0
        add.l   #$8000,d0
        swap    d0
        and.l   #$ff,d0
        or.l    d0,d1                   ; B
        move.l  d1,(a1)+
        move.l  (a4)+,d0                ; vi
        move.l  (a3)+,d1                ; vo
        move.l  d0,d2
        lsr.l   #8,d2
        and.l   #$ff00ff,d2
        add.l   d2,(a6,d4.l)
        move.l  d1,d2
        lsr.l   #8,d2
        and.l   #$ff00ff,d2
        sub.l   d2,(a6,d4.l)
        and.l   #$ff00ff,d0
        add.l   d0,(a6)
        and.l   #$ff00ff,d1
        sub.l   d1,(a6)+
        subq.l  #1,d3
        bne     .px
        addq.l  #1,d5
        cmp.l   60(sp),d5
        blt     .row
        movem.l (sp)+,d2-d7/a2-a6
        rts
