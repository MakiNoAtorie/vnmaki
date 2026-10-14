#!/usr/bin/env python3
"""Generate the audio test fixtures.

Usage: audio_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.

  files/      one short clip per format, for the decoder and mixer tests
  seeds/      the smallest clip of each format: the corpus the decoder fuzz target starts from
  game/       a project that plays audio, with game.transcript beside it
  missing/    a project that plays a file that is not there

WAV files are written here. The standard library cannot encode Opus, Vorbis or FLAC, so those
clips are stored below as base64. They were made once with ffmpeg 8.0.1 from its 440 Hz test
tone (amplitude 1/8), a quarter of a second, mono:

  ffmpeg -f lavfi -i sine=frequency=440:duration=0.25:sample_rate=RATE -ac 1 \\
         -map_metadata -1 -fflags +bitexact -flags:a +bitexact CODEC [-metadata TAG=N] FILE

  TONE_OPUS    RATE 48000, -c:a libopus -b:a 12k
  TONE_VORBIS  RATE 48000, -c:a libvorbis -q:a 0
  TONE_FLAC    RATE 8000,  -c:a flac -sample_fmt s16 -compression_level 12, then the 8 KB
               PADDING metadata block ffmpeg adds was removed
  LOOP_OPUS    as TONE_OPUS, with the tags LOOPSTART=4800 and LOOPLENGTH=4800
  LOOP_VORBIS  as TONE_VORBIS, with the tags LOOPSTART=2400 and LOOPEND=9600
"""

import base64
import math
import shutil
import struct
import sys
import wave
from pathlib import Path

TONE_OPUS = """
T2dnUwACAAAAAAAAAAAAAAAAAAAAAAIotXIBE09wdXNIZWFkAQE4AYC7AAAAAABPZ2dTAAAAAAAAAAAAAAAAAAABAAAA
SZW+VAEuT3B1c1RhZ3MGAAAAZmZtcGVnAQAAABQAAABlbmNvZGVyPUxhdmMgbGlib3B1c09nZ1MABBgwAAAAAAAAAAAA
AAIAAAC3MbtwDTMtKCkhKCwoKSszKylIgi63bFa39AAB5c2d7Rqm6QXqFZBnYqQLkptcTJU3EEPryGUs8PyDMNiH21kI
nX4kEqBIpIhXrJiFA1wmCZPn0jAIBRG9gFmHyICwK0fsWCaKq82dGUl2s6aTevgoephInBtSUUUArOMPxbifySZ8fLu4
02Ur1ZxYYDbUSiBnzd+/zyuvbFvZSJwbUlbOH+oRQROwZGn/HVtWAJUlLQKPhrgiyVbOuXsxbTnlNTufSbBInBufdZz8
STO/Qf+3kP8asY+xSkakHHCcLQpKUC7FF5BInBtXS922lJqIJKEsdkRjHPz6s8FiplhKLKGlSfhTSXPB/UKCgs2WSJwb
n3Wc/EXvX0sIP0+xsHOxfQ0yWcl5Ia3p1X/m8R25BzOqY6S0Xvgir5RInBtSUUUArOLUjH/euxAlT52tYEIr+uU28VtC
Hm2o18OSEQw47LTASJwbUlbOH+oGyVmafiD7rYOfcsBQFpq9Fbu9/6m/m83ixy+ugL6kRFBInBtRtBy/or3G9gVT/sLc
rgl+aG3FEvbkXwZA6WoU/l4W8TGYs/zwX9HASJwbV2baUe2stWIqtf9lyKxvqhLeVpcArkxxb1P2oMS2FTqS/UVKOwVX
8MK6Xy1NoO9ASJyTkbQcv6i2q9UkJGtKkEjl94RFFh11vgHM5sM7s2WBz+8C9G1jkqZiWUidrUCXAf3XqVVPSCano2wK
aQiQxqHzLsZD+R0BuqlKZKIOfcX3s0OA
"""

TONE_VORBIS = """
T2dnUwACAAAAAAAAAAAAAAAAAAAAAEU4l7QBHgF2b3JiaXMAAAAAAYC7AAAAAAAAgLsAAAAAAAC4AU9nZ1MAAAAAAAAA
AAAAAAAAAAEAAAAVcnjbDzD/////////////////MgN2b3JiaXMGAAAAZmZtcGVnAQAAABYAAABlbmNvZGVyPUxhdmMg
bGlidm9yYmlzAQV2b3JiaXMfQkNWAQAAAQAYY1QpRplS0kqJGXOUMUaZYpJKiaWEFkJInXMUU6k515xrrLm1IIQQGlNQ
KQWZUo5SaRljkCkFmVIQS0kldBI6J51jEFtJwdaYa4tBthyEDZpSTCnElFKKQggZU4wpxZRSSkIHJXQOOuYcU45KKEG4
nHOrtZaWY4updJJK5yRkTEJIKYWSSgelU05CSDWW1lIpHXNSUmpB6CCEEEK2IIQNgtCQVQAAAQDAQBAasgoAUAAAEIqh
GIoChIasAgAyAAAEoCiO4iiOIzmSY0kWEBqyCgAAAgAQAADAcBRJkRTJsSRL0ixL00RRVX3VNlVV9nVd13Vd13UgNGQV
AAABAEBIp5mlGiDCDGQYCA1ZBQAgAAAARijCEANCQ1YBAAABAABiKDmIJrTmfHOOg2Y5aCrF5nRwItXmSW4q5uacc845
J5tzxjjnnHOKcmYxaCa05pxzEoNmKWgmtOacc57E5kFrqrTmnHPGOaeDcUYY55xzmrTmQWo21uaccxa0pjlqLsXmnHMi
5eZJbS7V5pxzzjnnnHPOOeecc6oXp3NwTjjnnHOi9uZabkIX55xzPhmne3NCOOecc84555xzzjnnnHOC0JBVAAAQAABB
GDaGcacgSJ+jgRhFiGnIpAfdo8MkaAxyCqlHo6ORUuoglFTGSSmdIDRkFQAACAAAIYQUUkghhRRSSCGFFFKIIYYYYsgp
p5yCCiqppKKKMsoss8wyyyyzzDLrsLPOOuwwxBBDDK20EktNtdVYY62555xrDtJaaa211koppZRSSikIDVkFAIAAABAI
GWSQQUYhhRRSiCGmnHLKKaigAkJDVgEAgAAAAgAAADzJc0RHdERHdERHdERHdETHczxHlERJlERJtEzL1ExPFVXVlV1b
1mXd9m1hF3bd93Xf93Xj14VhWZZlWZZlWZZlWZZlWZZlWYLQkFUAAAgAAIAQQgghhRRSSCGlGGPMMeegk1BCIDRkFQAA
CAAgAAAAwFEcxXEkR3IkyZIsSZM0S7M8zdM8TfREURRN01RFV3RF3bRF2ZRN13RN2XRVWbVdWbZt2dZtX5Zt3/d93/d9
3/d93/d93/d1HQgNWQUASAAA6EiOpEiKpEiO4ziSJAGhIasAABkAAAEAKIqjOI7jSJIkSZakSZ7lWaJmaqZneqqoAqEh
qwAAQAAAAQAAAAAAKJriKabiKaLiOaIjSqJlWqKmaq4om7Lruq7ruq7ruq7ruq7ruq7ruq7ruq7ruq7ruq7ruq7ruq7r
ui4QGrIKAJAAANCRHMmRHEmRFEmRHMkBQkNWAQAyAAACAHAMx5AUybEsS9M8zdM8TfRET/RMTxVd0QVCQ1YBAIAAAAIA
AAAAADAkw1IsR3M0SZRUS7VUTbVUSxVVT1VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVTVN0zRNIDRkJQAA
BADAYo3B5SAhJSXl3hDCEJOeMSYhtV4hBJGS3jEGFYOeMqIMct5C4xCDHggNWREARAEAAMYgxxBzyDlHqZMSOeeodJQa
5xyljlJnKcWYYs0oldhSrI1zjlJHraOUYiwtdpRSjanGAgAAAhwAAAIshEJDVgQAUQAAhDFIKaQUYow5p5xDjCnnmHOG
MeYcc44556B0UirnnHROSsQYc445p5xzUjonlXNOSiehAACAAAcAgAALodCQFQFAnACAQZI8T/I0UZQ0TxRFU3RdUTRd
1/I81fRMU1U90VRVU1Vt2VRVWZY8zzQ901RVzzRV1VRVWTZVVZZFVdVt03V123RV3ZZt2/ddWxZ2UVVt3VRd2zdV1/Zd
2fZ9WdZ1Y/I8VfVM03U903Rl1XVtW3VdXfdMU5ZN15Vl03Vt25VlXXdl2fc103Rd01Vl2XRd2XZlV7ddWfZ903WF35Vl
X1dlWRh2XfeFW9eV5XRd3VdlVzdWWfZ9W9eF4dZ1YZk8T1U903RdzzRdV3VdX1dd19Y105Rl03Vt2VRdWXZl2fddV9Z1
zzRl2XRd2zZdV5ZdWfZ9V5Z13XRdX1dlWfhVV/Z1WdeV4dZt4Tdd1/dVWfaFV5Z14dZ1Ybl1XRg+VfV9U3aF4XRl39eF
31luXTiW0XV9YZVt4VhlWTl+4ViW3feVZXRdX1ht2RhWWRaGX/id5fZ943h1XRlu3efMuu8Mx++k+8rT1W1jmX3dWWZf
d47hGDq/8OOpqq+brisMpywLv+3rxrP7vrKMruv7qiwLvyrbwrHrvvP8vrAso+z6wmrLwrDatjHcvm4sv3Acy2vryjHr
vlG2dXxfeArD83R1XXlmXcf2dXTjRzh+ygAAgAEHAIAAE8pAoSErAoA4AQCPJImiZFmiKFmWKIqm6LqiaLqupGmmqWme
aVqaZ5qmaaqyKZquLGmaaVqeZpqap5mmaJqua5qmrIqmKcumasqyaZqy7LqybbuubNuiacqyaZqybJqmLLuyq9uu7Oq6
pFmmqXmeaWqeZ5qmasqyaZquq3meanqeaKqeKKqqaqqqraqqLFueZ5qa6KmmJ4qqaqqmrZqqKsumqtqyaaq2bKqqbbuq
7Pqybeu6aaqybaqmLZuqatuu7OqyLNu6L2maaWqeZ5qa55mmaZqybJqqK1uep5qeKKqq5ommaqqqLJumqsqW55mqJ4qq
6omea5qqKsumatqqaZq2bKqqLZumKsuubfu+68qybqqqbJuqauumasqybMu+78qq7oqmKcumqtqyaaqyLduy78uyrPui
acqyaaqybaqqLsuybRuzbPu6aJqybaqmLZuqKtuyLfu6LNu678qub6uqrOuyLfu67vqucOu6MLyybPuqrPq6K9u6b+sy
2/Z9RNOUZVM1bdtUVVl2Zdn2Zdv2fdE0bVtVVVs2TdW2ZVn2fVm2bWE0Tdk2VVXWTdW0bVmWbWG2ZeF2Zdm3ZVv2ddeV
dV/XfePXZd3murLty7Kt+6qr+rbu+8Jw667wCgAAGHAAAAgwoQwUGrISAIgCAACMYYwxCI1SzjkHoVHKOecgZM5BCCGV
zDkIIZSSOQehlJQy5yCUklIIoZSUWgshlJRSawUAABQ4AAAE2KApsThAoSErAYBUAACD41iW55miatqyY0meJ4qqqaq2
7UiW54miaaqqbVueJ4qmqaqu6+ua54miaaqq6+q6aJqmqaqu67q6Lpqiqaqq67qyrpumqqquK7uy7Oumqqqq68quLPvC
qrquK8uybevCsKqu68qybNu2b9y6ruu+7/vCka3rui78wjEMRwEA4AkOAEAFNqyOcFI0FlhoyEoAIAMAgDAGIYMQQgYh
hJBSSiGllBIAADDgAAAQYEIZKDRkRQAQJwAAGEMppJRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkgppZRSSimllFJK
KaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkqppJRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRS
SimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWU
UkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSiml
lFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoplVJKKaWUUkoppZRSSiml
lFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkop
pZRSCgCQinAAkHowoQwUGrISAEgFAACMUUopxpyDEDHmGGPQSSgpYsw5xhyUklLlHIQQUmktt8o5CCGk1FJtmXNSWosx
5hgz56SkFFvNOYdSUoux5ppr7qS0VmuuNedaWqs115xzzbm0FmuuOdecc8sx15xzzjnnGHPOOeecc84FAOA0OACAHtiw
OsJJ0VhgoSErAYBUAAACGaUYc8456BBSjDnnHIQQIoUYc845CCFUjDnnHHQQQqgYc8w5CCGEkDnnHIQQQgghcw466CCE
EEIHHYQQQgihlM5BCCGEEEooIYQQQgghhBA6CCGEEEIIIYQQQgghhFJKCCGEEEIJoZRQAABggQMAQIANqyOcFI0FFhqy
EgAAAgCAHJagUs6EQY5Bjw1BylEzDUJMOdGZYk5qMxVTkDkQnXQSGWpB2V4yCwAAgCAAIMAEEBggKPhCCIgxAABBiMwQ
CYVVsMCgDBoc5gHAA0SERACQmKBIu7iALgNc0MVdB0IIQhCCWBxAAQk4OOGGJ97whBucoFNU6iAAAAAAAAoA4AEA4KAA
IiKaq7C4wMjQ2ODo8AgAAAAAABUA+AAAOD6AiIjmKiwuMDI0Njg6PAIAAAAAAAAAAICAgAAAAAAAQAAAAICAT2dnUwAE
4C4AAAAAAAAAAAAAAgAAALyvCSYOFiwiIyMjIyIiIyMiTEZs7/vXZ/1XCQSDSosttviohqgqSXYAGqo+w7sAvp6dCW+o
KUJCFAAAAAAAAACA+y0BoN26fPlj8XsiYNu2vb5uAwAeqj6tmwAy36MD3yiNmuJJAQAAAAAAAAAQMwMAKOyZIwEAHqo+
rbsAMu+jB+8oAzXFkwIAAAAAAAAAYPh4AAABzz9kAAAeqj6tuwAy76MD7ygDNcWTAgAAAAAAAABgvQEA4HDHy2UAAB6q
Pq27ADLvowPvKAM1xZMCAAAAAAAAAKC/DQAQiTtXOgAAHqo+rbsAMu+jA+8oAzXFkwIAAAAAAAAAYHOpAAAC9h61AQAe
qj6tmwAy36MD3yiNmuJJAQAAAAAAAAAQUwMAKKx9IQEAHqo+rZsAMt+jB98oCzXFkwIAAAAAAAAAoH8bAECA+Z4eAB6q
Pq27ADLvowPvKDtqiicFAAAAAAAAAMB6AgDR4e7eVwAAHqo+rbsAMu+jA+8oAzXFkwIAAAAAAAAAoH8HAAAFC28TAAAe
qj6tuwAy76MD7yiNmuJJAQAAAAAAAACQbQEABAy5LgMAHoq+KHcBpU57wGT6hoqa4jHbgVf1BBCJUAAAAGz5t93l/X6X
7jK2CYDvx++x8fR+urI5XdmcHn+Pjb/Hxiub05XNdtvgJKazAOgwAV7o3ZysAtDTdg/o3lBTPAFhO5/rxxsICwAAAADQ
eW53GG7MuXl0v5JcFI3dlWN3FyvJxUpykcfuyrFgamGfdjvaFVACaAA=
"""

TONE_FLAC = """
ZkxhQwAAACICQAJAAACBAAC3AfQA8AAAB9Bzo0TVmdkj1rWk/tYV91ERhAAADgYAAABmZm1wZWcAAAAA//gkCADKQgAA
BWvmvDbAAgATDhk8wshQmTKWHDOGTOczJJlD85hQmTOZMkw5zlJTkmUJoWcwwlJTJTmTJmfMKSSZMoWcyZzykyZMOGTz
CyFCZMpYcM4ZM5zMkmUPzmFCZM5kyTDnOUlOSZQmhZzDCUlMlOZMmZ8wpJJkyhZzJnPKTJkw4ZPMLIUJkylhwzhkznMy
SZQ/OYUJkzmTJMOc5SU5JlCaFnMMJSUyU5kyZnzCkkmTKHFl//gkCAHNQvGH8BPmvDbAAgBOZM55SZMmHDJ5hZChMmUs
OGcMmc5mSTKH5zChMmcyZJhznKSnJMoTQs5hhKSmSnMmTM+YUkkyZQs5kznlJkyYcMnmFkKEyZSw4ZwyZzmZJMofnMKE
yZzJkmHOcpKckyhNCzmGEpKZKcyZMz5hSSTJlCzmTOeUmTJhwyeYWQoTJlLDhnDJnOZkkyh+cwoTJnMmSYc5ykpyTKE0
LOYYSkpkpzJkBUb/+CQIAsRCDFQIJea8NsACABnzCkkmTKFnMmc8pMmTDhk8wshQmTKWHDOGTOczJJlD85hQmTOZMkw5
zlJTkmUJoWcwwlJTJTmTJmfMKSSZMoWcyZzykyZMOGTzCyFCZMpYcM4ZM5zMkmUPzmFCZM5kyTDnOUlOSZQmhZzDCUlM
lOZMmZ8wpJJkyhZzJnPKTJkw4ZPMLIUJkylhwzhkznMySZQ/OYUJkzmTJMOc5SU5JlCaFnDdyf/4dAgDAQ8QQgP6CP3m
vC/ADAABIEMJGKjyQhCEM6rQwhCBCQljFsJCBCEk1jpCQgSASQuKNDIQCQISbGKWBIEIQJJ1VMkIQhIUqi2EgSBCQlor
yEJAkhtGDoSQIQJCmgxchIEgQwkYqPJCEIQzqtDCEIEJCWMWwkIEISTWOkDdGQ==
"""

LOOP_OPUS = """
T2dnUwACAAAAAAAAAAAAAAAAAAAAAAIotXIBE09wdXNIZWFkAQE4AYC7AAAAAABPZ2dTAAAAAAAAAAAAAAAAAAABAAAA
bM7zGQFTT3B1c1RhZ3MGAAAAZmZtcGVnAwAAABQAAABlbmNvZGVyPUxhdmMgbGlib3B1cw4AAABMT09QU1RBUlQ9NDgw
MA8AAABMT09QTEVOR1RIPTQ4MDBPZ2dTAAQYMAAAAAAAAAAAAAACAAAAtzG7cA0zLSgpISgsKCkrMyspSIIut2xWt/QA
AeXNne0apukF6hWQZ2KkC5KbXEyVNxBD68hlLPD8gzDYh9tZCJ1+JBKgSKSIV6yYhQNcJgmT59IwCAURvYBZh8iAsCtH
7FgmiqvNnRlJdrOmk3r4KHqYSJwbUlFFAKzjD8W4n8kmfHy7uNNlK9WcWGA21EogZ83fv88rr2xb2UicG1JWzh/qEUET
sGRp/x1bVgCVJS0Cj4a4IslWzrl7MW055TU7n0mwSJwbn3Wc/Ekzv0H/t5D/GrGPsUpGpBxwnC0KSlAuxReQSJwbV0vd
tpSaiCShLHZEYxz8+rPBYqZYSiyhpUn4U0lzwf1CgoLNlkicG591nPxF719LCD9PsbBzsX0NMlnJeSGt6dV/5vEduQcz
qmOktF74Iq+USJwbUlFFAKzi1Ix/3rsQJU+drWBCK/rlNvFbQh5tqNfDkhEMOOy0wEicG1JWzh/qBslZmn4g+62Dn3LA
UBaavRW7vf+pv5vN4scvroC+pERQSJwbUbQcv6K9xvYFU/7C3K4JfmhtxRL25F8GQOlqFP5eFvExmLP88F/RwEicG1dm
2lHtrLViKrX/Zcisb6oS3laXAK5McW9T9qDEthU6kv1FSjsFV/DCul8tTaDvQEick5G0HL+otqvVJCRrSpBI5feERRYd
db4BzObDO7Nlgc/vAvRtY5KmYllIna1AlwH916lVT0gmp6NsCmkIkMah8y7GQ/kdAbqpSmSiDn3F97NDgA==
"""

LOOP_VORBIS = """
T2dnUwACAAAAAAAAAAAAAAAAAAAAAEU4l7QBHgF2b3JiaXMAAAAAAYC7AAAAAAAAgLsAAAAAAAC4AU9nZ1MAAAAAAAAA
AAAAAAAAAAEAAAAxJnvvD1L/////////////////MgN2b3JiaXMGAAAAZmZtcGVnAwAAABYAAABlbmNvZGVyPUxhdmMg
bGlidm9yYmlzDgAAAExPT1BTVEFSVD0yNDAwDAAAAExPT1BFTkQ9OTYwMAEFdm9yYmlzH0JDVgEAAAEAGGNUKUaZUtJK
iRlzlDFGmWKSSomlhBZCSJ1zFFOpOdeca6y5tSCEEBpTUCkFmVKOUmkZY5ApBZlSEEtJJXQSOiedYxBbScHWmGuLQbYc
hA2aUkwpxJRSikIIGVOMKcWUUkpCByV0DjrmHFOOSihBuJxzq7WWlmOLqXSSSuckZExCSCmFkkoHpVNOQkg1ltZSKR1z
UlJqQegghBBCtiCEDYLQkFUAAAEAwEAQGrIKAFAAABCKoRiKAoSGrAIAMgAABKAojuIojiM5kmNJFhAasgoAAAIAEAAA
wHAUSZEUybEkS9IsS9NEUVV91TZVVfZ1Xdd1Xdd1IDRkFQAAAQBASKeZpRogwgxkGAgNWQUAIAAAAEYowhADQkNWAQAA
AQAAYig5iCa05nxzjoNmOWgqxeZ0cCLV5kluKubmnHPOOSebc8Y455xzinJmMWgmtOaccxKDZiloJrTmnHOexOZBa6q0
5pxzxjmng3FGGOecc5q05kFqNtbmnHMWtKY5ai7F5pxzIuXmSW0u1eacc84555xzzjnnnHOqF6dzcE4455xzovbmWm5C
F+eccz4Zp3tzQjjnnHPOOeecc84555xzgtCQVQAAEAAAQRg2hnGnIEifo4EYRYhpyKQH3aPDJGgMcgqpR6OjkVLqIJRU
xkkpnSA0ZBUAAAgAACGEFFJIIYUUUkghhRRSiCGGGGLIKaecggoqqaSiijLKLLPMMssss8wy67CzzjrsMMQQQwyttBJL
TbXVWGOtueecaw7SWmmttdZKKaWUUkopCA1ZBQCAAAAQCBlkkEFGIYUUUoghppxyyimooAJCQ1YBAIAAAAIAAAA8yXNE
R3RER3RER3RER3REx3M8R5RESZRESbRMy9RMTxVV1ZVdW9Zl3fZtYRd23fd13/d149eFYVmWZVmWZVmWZVmWZVmWZVmC
0JBVAAAIAACAEEIIIYUUUkghpRhjzDHnoJNQQiA0ZBUAAAgAIAAAAMBRHMVxJEdyJMmSLEmTNEuzPM3TPE30RFEUTdNU
RVd0Rd20RdmUTdd0Tdl0VVm1XVm2bdnWbV+Wbd/3fd/3fd/3fd/3fd/3dR0IDVkFAEgAAOhIjqRIiqRIjuM4kiQBoSGr
AAAZAAABACiKoziO40iSJEmWpEme5VmiZmqmZ3qqqAKhIasAAEAAAAEAAAAAACia4imm4imi4jmiI0qiZVqipmquKJuy
67qu67qu67qu67qu67qu67qu67qu67qu67qu67qu67qu67ouEBqyCgCQAADQkRzJkRxJkRRJkRzJAUJDVgEAMgAAAgBw
DMeQFMmxLEvTPM3TPE30RE/0TE8VXdEFQkNWAQCAAAACAAAAAAAwJMNSLEdzNEmUVEu1VE21VEsVVU9VVVVVVVVVVVVV
VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVU1TdM0TSA0ZCUAAAQAwGKNweUgISUl5d4QwhCTnjEmIbVeIQSRkt4xBhWDnjKi
DHLeQuMQgx4IDVkRAEQBAADGIMcQc8g5R6mTEjnnqHSUGuccpY5SZynFmGLNKJXYUqyNc45SR62jlGIsLXaUUo2pxgIA
AAIcAAACLIRCQ1YEAFEAAIQxSCmkFGKMOaecQ4wp55hzhjHmHHOOOeegdFIq55x0TkrEGHOOOaecc1I6J5VzTkonoQAA
gAAHAIAAC6HQkBUBQJwAgEGSPE/yNFGUNE8URVN0XVE0XdfyPNX0TFNVPdFUVVNVbdlUVVmWPM80PdNUVc80VdVUVVk2
VVWWRVXVbdN1ddt0Vd2Wbdv3XVsWdlFVbd1UXds3Vdf2Xdn2fVnWdWPyPFX1TNN1PdN0ZdV1bVt1XV33TFOWTdeVZdN1
bduVZV13Zdn3NdN0XdNVZdl0Xdl2ZVe3XVn2fdN1hd+VZV9XZVkYdl33hVvXleV0Xd1XZVc3Vln2fVvXheHWdWGZPE9V
PdN0Xc80XVd1XV9XXdfWNdOUZdN1bdlUXVl2Zdn3XVfWdc80Zdl0Xds2XVeWXVn2fVeWdd10XV9XZVn4VVf2dVnXleHW
beE3Xdf3VVn2hVeWdeHWdWG5dV0YPlX1fVN2heF0Zd/Xhd9Zbl04ltF1fWGVbeFYZVk5fuFYlt33lWV0XV9YbdkYVlkW
hl/4neX2feN4dV0Zbt3nzLrvDMfvpPvK09VtY5l93VlmX3eO4Rg6v/Djqaqvm64rDKcsC7/t68az+76yjK7r+6osC78q
28Kx677z/L6wLKPs+sJqy8Kw2rYx3L5uLL9wHMtr68ox675RtnV8X3gKw/N0dV15Zl3H9nV040c4fsoAAIABBwCAABPK
QKEhKwKAOAEAjySJomRZoihZliiKpui6omi6rqRppqlpnmlammeapmmqsimarixpmmlanmaamqeZpmiarmuapqyKpinL
pmrKsmmasuy6sm27rmzbomnKsmmasmyapiy7sqvbruzquqRZpql5nmlqnmeapmrKsmmarqt5nmp6nmiqniiqqmqqqq2q
qixbnmeamuippieKqmqqpq2aqirLpqrasmmqtmyqqm27quz6sm3rummqsm2qpi2bqmrbruzqsizbui9pmmlqnmeamueZ
pmmasmyaqitbnqeaniiqquaJpmqqqiybpqrKlueZqieKquqJnmuaqirLpmraqmmatmyqqi2bpirLrm37vuvKsm6qqmyb
qmrrpmrKsmzLvu/Kqu6KpinLpqrasmmqsi3bsu/Lsqz7omnKsmmqsm2qqi7Lsm0bs2z7umiasm2qpi2bqirbsi37uizb
uu/Krm+rqqzrsi37uu76rnDrujC8smz7qqz6uivbum/rMtv2fUTTlGVTNW3bVFVZdmXZ9mXb9n3RNG1bVVVbNk3VtmVZ
9n1Ztm1hNE3ZNlVV1k3VtG1Zlm1htmXhdmXZt2Vb9nXXlXVf133j12Xd5rqy7cuyrfuqq/q27vvCcOuu8AoAABhwAAAI
MKEMFBqyEgCIAgAAjGGMMQiNUs45B6FRyjnnIGTOQQghlcw5CCGUkjkHoZSUMucglJJSCKGUlFoLIZSUUmsFAAAUOAAA
BNigKbE4QKEhKwGAVAAAg+NYlueZomrasmNJnieKqqmqtu1IlueJommqqm1bnieKpqmqruvrmueJommqquvqumiapqmq
ruu6ui6aoqmqquu6sq6bpqqqriu7suzrpqqqquvKriz7wqq6rivLsm3rwrCqruvKsmzbtm/cuq7rvu/7wpGt67ou/MIx
DEcBAOAJDgBABTasjnBSNBZYaMhKACADAIAwBiGDEEIGIYSQUkohpZQSAAAw4AAAEGBCGSg0ZEUAECcAABhDKaSUUkop
pZRSSimllFJKKaWUUkoppZRSSimllFJIKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJK
qaSUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRS
SimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWU
UkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSiml
lFJKKaWUUkoppZRSSimllFJKKZVSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSiml
lFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUgoAkIpwAJB6MKEMFBqyEgBIBQAAjFFKKcacgxAx5hhj
0EkoKWLMOcYclJJS5RyEEFJpLbfKOQghpNRSbZlzUlqLMeYYM+ekpBRbzTmHUlKLseaaa+6ktFZrrjXnWlqrNdecc825
tBZrrjnXnHPLMdecc8455xhzzjnnnHPOBQDgNDgAgB7YsDrCSdFYYKEhKwGAVAAAAhmlGHPOOegQUow55xyEECKFGHPO
OQghVIw55xx0EEKoGHPMOQghhJA55xyEEEIIIXMOOugghBBCBx2EEEIIoZTOQQghhBBKKCGEEEIIIYQQOgghhBBCCCGE
EEIIIYRSSgghhBBCCaGUUAAAYIEDAECADasjnBSNBRYashIAAAIAgByWoFLOhEGOQY8NQcpRMw1CTDnRmWJOajMVU5A5
EJ10EhlqQdleMgsAAIAgACDABBAYICj4QgiIMQAAQYjMEAmFVbDAoAwaHOYBwANEhEQAkJigSLu4gC4DXNDFXQdCCEIQ
glgcQAEJODjhhife8IQbnKBTVOogAAAAAAAKAOABAOCgACIimquwuMDI0Njg6PAIAAAAAAAVAPgAADg+gIiI5iosLjAy
NDY4OjwCAAAAAAAAAACAgIAAAAAAAEAAAACAgE9nZ1MABOAuAAAAAAAAAAAAAAIAAAC8rwkmDhYsIiMjIyMiIiMjIkxG
bO/712f9VwkEg0qLLbb4qIaoKkl2ABqqPsO7AL6enQlvqClCQhQAAAAAAAAAgPstAaDdunz5Y/F7ImDbtr2+bgMAHqo+
rZsAMt+jA98ojZriSQEAAAAAAAAAEDMDACjsmSMBAB6qPq27ADLvowfvKAM1xZMCAAAAAAAAAGD4eAAAAc8/ZAAAHqo+
rbsAMu+jA+8oAzXFkwIAAAAAAAAAYL0BAOBwx8tlAAAeqj6tuwAy76MD7ygDNcWTAgAAAAAAAACgvw0AEIk7VzoAAB6q
Pq27ADLvowPvKAM1xZMCAAAAAAAAAGBzqQAAAvYetQEAHqo+rZsAMt+jA98ojZriSQEAAAAAAAAAEFMDACisfSEBAB6q
Pq2bADLfowffKAs1xZMCAAAAAAAAAKB/GwBAgPmeHgAeqj6tuwAy76MD7yg7aoonBQAAAAAAAADAegIA0eHu3lcAAB6q
Pq27ADLvowPvKAM1xZMCAAAAAAAAAKB/BwAABQtvEwAAHqo+rbsAMu+jA+8ojZriSQEAAAAAAAAAkG0BAAQMuS4DAB6K
vih3AaVOe8Bk+oaKmuIx24FX9QQQiVAAAABs+bfd5f1+l+4ytgmA78fvsfH0frqyOV3ZnB5/j42/x8Yrm9OVzXbb4CSm
swDoMAFe6N2crALQ03YP6N5QUzwBYTuf68cbCAsAAAAA0HludxhuzLl5dL+SXBSN3ZVjdxcrycVKcpHH7sqxYGphn3Y7
2hVQAmgA
"""

RATE = 48000
TONE_SECONDS = 0.25
LONG_SECONDS = 2.0
SEED_SECONDS = 0.01
AMPLITUDE = 0.5
INT16_MAX = 32767


def wav(path: Path, rate: int, seconds: float, hertz: float, channels: int = 1) -> None:
    """A sine tone. With two channels the right one is silent."""
    frames = bytearray()
    for i in range(round(rate * seconds)):
        sample = round(AMPLITUDE * INT16_MAX * math.sin(2 * math.pi * hertz * i / rate))
        frames += struct.pack("<h", sample)
        if channels == 2:
            frames += struct.pack("<h", 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


def encoded(path: Path, literal: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(base64.b64decode(literal))


def text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content.encode("utf-8"))


GAME_SCRIPT = """\
label("start", function()
  music("bgm/theme", { fade = 1.5 })
  narrate("Rain on the roof.")
  sound("sfx/door")
  voice("eileen/001")
  say("Eileen", "You came back.")
  volume("music", 0.5)
  music("bgm/night", { channel = "ambient", loop_start = 0.05, loop_end = 0.2 })
  stop("music", { fade = 2 })
  narrate("Silence.")
end)
"""

GAME_TRANSCRIPT = """\
label start
music bgm/theme fade 1.5
narrate Rain on the roof.
sound sfx/door
voice eileen/001
say Eileen: You came back.
volume music 0.5
music bgm/night channel ambient loop_start 0.05 loop_end 0.2
stop music fade 2
narrate Silence.
"""

MISSING_SCRIPT = """\
label("start", function()
  narrate("Quiet.")
  music("bgm/ghost")
end)
"""


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: audio_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)

    files = out / "files"
    wav(files / "tone.wav", RATE, TONE_SECONDS, 440)
    wav(files / "tone44.wav", 44100, TONE_SECONDS, 440, channels=2)
    wav(files / "long.wav", RATE, LONG_SECONDS, 220)
    encoded(files / "tone.opus", TONE_OPUS)
    encoded(files / "tone.ogg", TONE_VORBIS)
    encoded(files / "tone.flac", TONE_FLAC)
    encoded(files / "loop.opus", LOOP_OPUS)
    encoded(files / "loop.ogg", LOOP_VORBIS)

    seeds = out / "seeds"
    wav(seeds / "tone.wav", RATE, SEED_SECONDS, 440)
    encoded(seeds / "tone.opus", TONE_OPUS)
    encoded(seeds / "tone.ogg", TONE_VORBIS)
    encoded(seeds / "tone.flac", TONE_FLAC)
    encoded(seeds / "loop.opus", LOOP_OPUS)

    game = out / "game"
    text(game / "project.toml", 'title = "Audio"\n')
    text(game / "scripts/start.lua", GAME_SCRIPT)
    encoded(game / "audio/bgm/theme.opus", LOOP_OPUS)
    encoded(game / "audio/bgm/night.ogg", TONE_VORBIS)
    wav(game / "audio/sfx/door.wav", RATE, TONE_SECONDS, 440)
    encoded(game / "audio/voice/eileen/001.flac", TONE_FLAC)
    text(out / "game.transcript", GAME_TRANSCRIPT)

    missing = out / "missing"
    text(missing / "project.toml", 'title = "Missing audio"\n')
    text(missing / "scripts/start.lua", MISSING_SCRIPT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
