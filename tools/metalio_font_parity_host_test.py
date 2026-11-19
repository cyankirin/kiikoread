#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：两板内嵌资产逐字节一致、补字外置分发一致、旧界面像素不变与启动画布检查。
English: Byte-identical embedded assets across boards, identical distributed supplement,
unchanged legacy UI pixels and the actual startup canvas check.
"""
from pathlib import Path
import hashlib,json,struct,subprocess,tempfile,zlib
from font_hanzi_host_support import font_codepoints
from home_cover_cache_host_test import function
R=Path(__file__).resolve().parents[1]
marker=R/'.kiiko-source-view.json';source=Path(json.loads(marker.read_text())['source']) if marker.exists() else R
pico=source/'boards/pico/src/main/assets';s31=source/'boards/metalio47/src/main/assets'
native=(s31/'builtin.ttf').read_bytes()
# 两板内嵌 TTF 逐字节一致（剥除 name/OS/2/post 后），stb 只读的表保留。
# Board TTFs stay byte-identical after stripping name/OS/2/post; stb tables remain.
assert native==(pico/'builtin.ttf').read_bytes()
assert hashlib.sha256(native).hexdigest()=='bbca2dc72869dedc3a5fe9839b286ff5514cefbbfc3bcbd554f1f78015c489ce'
tables={native[12+i*16:16+i*16] for i in range(struct.unpack_from('>H',native,4)[0])}
assert not (tables & {b'name',b'OS/2',b'post'}) and {b'cmap',b'glyf',b'loca',b'head',b'hhea',b'hmtx',b'maxp'}<=tables
# 内嵌 PIF1 基线（降级路径）两板共用一份，逐字节 pin；板级 PIF4 覆盖副本已移除。
# The embedded PIF1 fallback baseline is one shared file; byte pin. Board PIF4 overlays are gone.
embedded=(source/'main/assets/ui-hanzi.bin').read_bytes()
emagic,ecount,epx,eblock=struct.unpack_from('<4sIII',embedded)
assert (emagic,ecount,epx,eblock)==(b'PIF1',5637,24,32)
assert hashlib.sha256(embedded).hexdigest()=='6c1b29d0786b673a6ee8ced773cf48b3b08c837b453219f15ff9e01ee7f0155b'
assert not (pico/'ui-hanzi.bin').exists() and not (s31/'ui-hanzi.bin').exists()
# 外置全量 PIF4 分发文件两板共用，逐字节 pin；其旧界面像素展开不变。
# The distributed external full PIF4 is shared by both boards; byte pin with unchanged legacy pixels.
distributed=sorted((source/'flash'/'fonts').glob('ui-hanzi-*.bin'));assert len(distributed)==1
aa=distributed[0].read_bytes()
magic,count,px,block=struct.unpack_from('<4sIII',aa);assert (magic,count,px,block)==(b'PIF4',6030,24,32)
assert hashlib.sha256(aa).hexdigest()=='184eea2a747098250b389a589b8244430793a064f837c22a1550f3fca4dd571e'
chars=struct.unpack_from('<'+'H'*count,aa,16);at=16+count*2
index=struct.unpack_from('<'+'I'*((count+31)//32+1),aa,at);at+=len(index)*4
mono=bytearray()
for n in range(len(index)-1):
 raw=zlib.decompress(aa[at+index[n]:at+index[n+1]])
 for off in range(0,len(raw),293):
  record=raw[off:off+293];old=bytearray(record[:5]+bytes(72))
  for bit in range(576):
   if ((record[5+bit//2]>>((bit%2)*4))&15)>=6:old[5+bit//8]|=1<<(bit%8)
  mono.extend(old)
assert hashlib.sha256(mono).hexdigest()=='c9ab41911c03df18540b84ed6aee9121022f59f758d3b5ae3b288005218c97b3'
covered=font_codepoints(native)|set(chars);assert len(covered)==7668
common={int(line.split('#',1)[0].strip(),16) for line in (source/'tools/fixtures/reader-common-6500.txt').read_text().splitlines() if line.split('#',1)[0].strip()};assert common<=covered
for name in ['reader_missing_advance','ttf_text_left_bearing_px']:
 assert function(name,source/'main/font/ttf_font.c')==function(name,source/'boards/pico/src/main/font/ttf_font.c'),name
cmake=(source/'main/CMakeLists.txt').read_text().split('\nelse()',1)[0];assert '"assets/lock_4bpp.bin"' not in cmake
boot=source/'boards/metalio47/src/main/app_main.c';assert '_binary_lock_4bpp' not in boot.read_text()
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define UI_LOCK_WIDTH 684
#define UI_LOCK_HEIGHT 1216
#define ESP_LOGE(...) ((void)0)
static int w,h;
static int epd_width(void){return w;}static int epd_height(void){return h;}
'''+function('canvas_matches_panel',boot)+r'''
int main(void){w=1216;h=684;assert(canvas_matches_panel());w=684;h=1216;assert(canvas_matches_panel());
 w=2432;h=342;assert(!canvas_matches_panel());w=1215;h=684;assert(!canvas_matches_panel());w=0;h=0;assert(!canvas_matches_panel());
 puts("PASS: actual S31 startup keeps both native orientations and rejects incompatible canvases without unused full-screen art");}
'''
with tempfile.TemporaryDirectory() as folder:
 p=Path(folder);(p/'test.c').write_text(unit)
 subprocess.run(['cc','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: boards share identical embedded TTF/PIF1, one distributed PIF4 supplement; 7668 characters/all common6500; all6030 legacy UI glyph pixels/metrics unchanged; exact advances/bearings shared')
