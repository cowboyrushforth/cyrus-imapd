#
# Copyright (c) 1994-2008 Carnegie Mellon University.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in
#    the documentation and/or other materials provided with the
#    distribution.
#
# 3. The name "Carnegie Mellon University" must not be used to
#    endorse or promote products derived from this software without
#    prior written permission. For permission or any legal
#    details, please contact
#      Carnegie Mellon University
#      Center for Technology Transfer and Enterprise Creation
#      4615 Forbes Avenue
#      Suite 302
#      Pittsburgh, PA  15213
#      (412) 268-7393, fax: (412) 268-7395
#      innovation@andrew.cmu.edu
#
# 4. Redistributions of any form whatsoever must retain the following
#    acknowledgment:
#    "This product includes software developed by Computing Services
#     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
#
# CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
# THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
# FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
# AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
# OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
# $Id: koi8-r.t,v 1.6 2010/01/06 17:01:49 murch Exp $
 0 0000 NULL (NUL)
 1 0001 START OF HEADING (SOH)
 2 0002 START OF TEXT (STX)
 3 0003 END OF TEXT (ETX)
 4 0004 END OF TRANSMISSION (EOT)
 5 0005 ENQUIRY (ENQ)
 6 0006 ACKNOWLEDGE (ACK)
 7 0007 BELL (BEL)
 8 0008 BACKSPACE (BS)
 9 0009 CHARACTER TABULATION (HT)
 A 000a LINE FEED (LF)
 B 000b LINE TABULATION (VT)
 C 000c FORM FEED (FF)
 D 000d CARRIAGE RETURN (CR)
 E 000e SHIFT OUT (SO)
 F 000f SHIFT IN (SI)
10 0010 DATALINK ESCAPE (DLE)
11 0011 DEVICE CONTROL ONE (DC1)
12 0012 DEVICE CONTROL TWO (DC2)
13 0013 DEVICE CONTROL THREE (DC3)
14 0014 DEVICE CONTROL FOUR (DC4)
15 0015 NEGATIVE ACKNOWLEDGE (NAK)
16 0016 SYNCRONOUS IDLE (SYN)
17 0017 END OF TRANSMISSION BLOCK (ETB)
18 0018 CANCEL (CAN)
19 0019 END OF MEDIUM (EM)
1A 001a SUBSTITUTE (SUB)
1B 001b ESCAPE (ESC)
1C 001c FILE SEPARATOR (IS4)
1D 001d GROUP SEPARATOR (IS3)
1E 001e RECORD SEPARATOR (IS2)
1F 001f UNIT SEPARATOR (IS1)
20 0020 SPACE
21 0021 EXCLAMATION MARK
22 0022 QUOTATION MARK
23 0023 NUMBER SIGN
24 0024 DOLLAR SIGN
25 0025 PERCENT SIGN
26 0026 AMPERSAND
27 0027 APOSTROPHE
28 0028 LEFT PARENTHESIS
29 0029 RIGHT PARENTHESIS
2A 002a ASTERISK
2B 002b PLUS SIGN
2C 002c COMMA
2D 002d HYPHEN-MINUS
2E 002e FULL STOP
2F 002f SOLIDUS
30 0030 DIGIT ZERO
31 0031 DIGIT ONE
32 0032 DIGIT TWO
33 0033 DIGIT THREE
34 0034 DIGIT FOUR
35 0035 DIGIT FIVE
36 0036 DIGIT SIX
37 0037 DIGIT SEVEN
38 0038 DIGIT EIGHT
39 0039 DIGIT NINE
3A 003a COLON
3B 003b SEMICOLON
3C 003c LESS-THAN SIGN
3D 003d EQUALS SIGN
3E 003e GREATER-THAN SIGN
3F 003f QUESTION MARK
40 0040 COMMERCIAL AT
41 0041 LATIN CAPITAL LETTER A
42 0042 LATIN CAPITAL LETTER B
43 0043 LATIN CAPITAL LETTER C
44 0044 LATIN CAPITAL LETTER D
45 0045 LATIN CAPITAL LETTER E
46 0046 LATIN CAPITAL LETTER F
47 0047 LATIN CAPITAL LETTER G
48 0048 LATIN CAPITAL LETTER H
49 0049 LATIN CAPITAL LETTER I
4A 004a LATIN CAPITAL LETTER J
4B 004b LATIN CAPITAL LETTER K
4C 004c LATIN CAPITAL LETTER L
4D 004d LATIN CAPITAL LETTER M
4E 004e LATIN CAPITAL LETTER N
4F 004f LATIN CAPITAL LETTER O
50 0050 LATIN CAPITAL LETTER P
51 0051 LATIN CAPITAL LETTER Q
52 0052 LATIN CAPITAL LETTER R
53 0053 LATIN CAPITAL LETTER S
54 0054 LATIN CAPITAL LETTER T
55 0055 LATIN CAPITAL LETTER U
56 0056 LATIN CAPITAL LETTER V
57 0057 LATIN CAPITAL LETTER W
58 0058 LATIN CAPITAL LETTER X
59 0059 LATIN CAPITAL LETTER Y
5A 005a LATIN CAPITAL LETTER Z
5B 005b LEFT SQUARE BRACKET
5C 005c REVERSE SOLIDUS
5D 005d RIGHT SQUARE BRACKET
5E 005e CIRCUMFLEX ACCENT
5F 005f LOW LINE
60 0060 GRAVE ACCENT
61 0061 LATIN SMALL LETTER A
62 0062 LATIN SMALL LETTER B
63 0063 LATIN SMALL LETTER C
64 0064 LATIN SMALL LETTER D
65 0065 LATIN SMALL LETTER E
66 0066 LATIN SMALL LETTER F
67 0067 LATIN SMALL LETTER G
68 0068 LATIN SMALL LETTER H
69 0069 LATIN SMALL LETTER I
6A 006a LATIN SMALL LETTER J
6B 006b LATIN SMALL LETTER K
6C 006c LATIN SMALL LETTER L
6D 006d LATIN SMALL LETTER M
6E 006e LATIN SMALL LETTER N
6F 006f LATIN SMALL LETTER O
70 0070 LATIN SMALL LETTER P
71 0071 LATIN SMALL LETTER Q
72 0072 LATIN SMALL LETTER R
73 0073 LATIN SMALL LETTER S
74 0074 LATIN SMALL LETTER T
75 0075 LATIN SMALL LETTER U
76 0076 LATIN SMALL LETTER V
77 0077 LATIN SMALL LETTER W
78 0078 LATIN SMALL LETTER X
79 0079 LATIN SMALL LETTER Y
7A 007a LATIN SMALL LETTER Z
7B 007b LEFT CURLY BRACKET
7C 007c VERTICAL LINE
7D 007d RIGHT CURLY BRACKET
7E 007e TILDE
7F 007f DELETE (DEL)
80 2500 FORMS LIGHT HORIZONTAL
81 2502 FORMS LIGHT VERTICAL
82 250c FORMS LIGHT DOWN AND RIGHT
83 2510 FORMS LIGHT DOWN AND LEFT
84 2514 FORMS LIGHT UP
85 2518 FORMS LIGHT UP AND LEFT
86 251c FORMS LIGHT VERTICAL AND RIGHT
87 2524 FORMS LIGHT VERTICAL AND LEFT
88 252c FORMS LIGHT DOWN AND HORIZONTAL
89 2534 FORMS LIGHT UP AND HORIZONTAL
8A 253c FORMS LIGHT VERTICAL AND HORIZONTAL
8B 2580 UPPER HALF BLOCK
8C 2584 LOWER HALF BLOCK
8D 2588 FULL BLOCK
8E 258c LEFT HALF BLOCK
8F 2590 RIGHT HALF BLOCK
90 2591 LIGHT SHADE
91 2592 MEDIUM SHADE
92 2593 DARK SHADE
93 2320 TOP HALF INTEGRAL
94 25a0 BLACK SMALL SQUARE
95 2219 BULLET OPERATOR
96 221a SQUARE ROOT
97 2248 ALMOST EQUAL TO
98 2264 LESS THAN OR EQUAL TO
99 2265 GREATER THAN OR EQUAL TO
9A 00a0 NON-BREAKING SPACE
9B 2321 BOTTOM HALF INTEGRAL
9C 00b0 DEGREE SIGN
9D 00b2 SUPERSCRIPT DIGIT TWO
9E 00b7 MIDDLE DOT
9F 00f7 DIVISION SIGN
A0 2550 FORMS DOUBLE HORIZONTAL
A1 2551 FORMS DOUBLE VERTICAL
A2 2552 FORMS DOWN SINGLE AND RIGHT DOUBLE
A3 0451 CYRILLIC SMALL LETTER IO
A4 2553 FORMS DOWN DOUBLE AND RIGHT SINGLE
A5 2554 FORMS DOUBLE DOWN AND RIGHT
A6 2555 FORMS DOWN SINGLE AND LEFT DOUBLE
A7 2556 FORMS DOWN DOUBLE AND LEFT SINGLE
A8 2557 FORMS DOUBLE DOWN AND LEFT
A9 2558 FORMS UP SINGLE AND RIGHT DOUBLE
AA 2559 FORMS UP DOUBLE AND RIGHT SINGLE
AB 255a FORMS DOUBLE UP AND RIGHT
AC 255b FORMS UP SINGLE AND LEFT DOUBLE
AD 255c FORMS UP DOUBLE AND LEFT SINGLE
AE 255d FORMS DOUBLE UP AND LEFT
AF 255e FORMS VERTICAL SINGLE AND RIGHT DOUBLE
B0 255f FORMS VERTICAL DOUBLE AND RIGHT SINGLE
B1 2560 FORMS DOUBLE VERTICAL AND RIGHT
B2 2561 FORMS VERTICAL SINGLE AND LEFT DOUBLE
B3 0401 CYRILLIC CAPITAL LETTER IO
B4 2562 FORMS VERTICAL DOUBLE AND LEFT SINGLE
B5 2563 FORMS DOUBLE VERTICAL AND LEFT
B6 2564 FORMS DOWN SINGLE AND HORIZONTAL DOUBLE
B7 2565 FORMS DOWN DOUBLE AND HORIZONTAL SINGLE
B8 2566 FORMS DOUBLE DOWN AND HORIZONTAL
B9 2567 FORMS UP SINGLE AND HORIZONTAL DOUBLE
BA 2568 FORMS UP DOUBLE AND HORIZONTAL SINGLE
BB 2569 FORMS DOUBLE UP AND HORIZONTAL
BC 256a FORMS VERTICAL SINGLE AND HORIZONTAL DOUBLE
BD 256b FORMS VERTICAL DOUBLE AND HORIZONTAL SINGLE
BE 256c FORMS DOUBLE VERTICAL AND HORIZONTAL
BF 00a9 COPYRIGHT SIGN
C0 044e CYRILLIC SMALL LETTER IU
C1 0430 CYRILLIC SMALL LETTER A
C2 0431 CYRILLIC SMALL LETTER BE
C3 0446 CYRILLIC SMALL LETTER TSE
C4 0434 CYRILLIC SMALL LETTER DE
C5 0435 CYRILLIC SMALL LETTER IE
C6 0444 CYRILLIC SMALL LETTER EF
C7 0433 CYRILLIC SMALL LETTER GE
C8 0445 CYRILLIC SMALL LETTER KHA
C9 0438 CYRILLIC SMALL LETTER II
CA 0439 CYRILLIC SMALL LETTER SHORT II
CB 043a CYRILLIC SMALL LETTER KA
CC 043b CYRILLIC SMALL LETTER EL
CD 043c CYRILLIC SMALL LETTER EM
CE 043d CYRILLIC SMALL LETTER EN
CF 043e CYRILLIC SMALL LETTER O
D0 043f CYRILLIC SMALL LETTER PE
D1 044f CYRILLIC SMALL LETTER IA
D2 0440 CYRILLIC SMALL LETTER ER
D3 0441 CYRILLIC SMALL LETTER ES
D4 0442 CYRILLIC SMALL LETTER TE
D5 0443 CYRILLIC SMALL LETTER U
D6 0436 CYRILLIC SMALL LETTER ZHE
D7 0432 CYRILLIC SMALL LETTER VE
D8 044c CYRILLIC SMALL LETTER SOFT SIGN
D9 044b CYRILLIC SMALL LETTER YERI
DA 0437 CYRILLIC SMALL LETTER ZE
DB 0448 CYRILLIC SMALL LETTER SHA
DC 044d CYRILLIC SMALL LETTER REVERSED E
DD 0449 CYRILLIC SMALL LETTER SHCHA
DE 0447 CYRILLIC SMALL LETTER CHE
DF 044a CYRILLIC SMALL LETTER HARD SIGN
E0 042e CYRILLIC CAPITAL LETTER IU
E1 0410 CYRILLIC CAPITAL LETTER A
E2 0411 CYRILLIC CAPITAL LETTER BE
E3 0426 CYRILLIC CAPITAL LETTER TSE
E4 0414 CYRILLIC CAPITAL LETTER DE
E5 0415 CYRILLIC CAPITAL LETTER IE
E6 0424 CYRILLIC CAPITAL LETTER EF
E7 0413 CYRILLIC CAPITAL LETTER GE
E8 0425 CYRILLIC CAPITAL LETTER KHA
E9 0418 CYRILLIC CAPITAL LETTER II
EA 0419 CYRILLIC CAPITAL LETTER SHORT II
EB 041a CYRILLIC CAPITAL LETTER KA
EC 041b CYRILLIC CAPITAL LETTER EL
ED 041c CYRILLIC CAPITAL LETTER EM
EE 041d CYRILLIC CAPITAL LETTER EN
EF 041e CYRILLIC CAPITAL LETTER O
F0 041f CYRILLIC CAPITAL LETTER PE
F1 042f CYRILLIC CAPITAL LETTER IA
F2 0420 CYRILLIC CAPITAL LETTER ER
F3 0421 CYRILLIC CAPITAL LETTER ES
F4 0422 CYRILLIC CAPITAL LETTER TE
F5 0423 CYRILLIC CAPITAL LETTER U
F6 0416 CYRILLIC CAPITAL LETTER ZHE
F7 0412 CYRILLIC CAPITAL LETTER VE
F8 042c CYRILLIC CAPITAL LETTER SOFT SIGN
F9 042b CYRILLIC CAPITAL LETTER YERI
FA 0417 CYRILLIC CAPITAL LETTER ZE
FB 0428 CYRILLIC CAPITAL LETTER SHA
FC 042d CYRILLIC CAPITAL LETTER REVERSED E
FD 0429 CYRILLIC CAPITAL LETTER SHCHA
FE 0427 CYRILLIC CAPITAL LETTER CHE
FF 042a CYRILLIC CAPITAL LETTER HARD SIGN
