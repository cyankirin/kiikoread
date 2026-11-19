#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 中文：验证实际汉字补充解码、外置/内嵌双模式装载、字形覆盖与栈界限。
#   外置 PIF4（flash/fonts/ 分发文件）走文件装载路径；内嵌 PIF1 是文件缺失时的降级基线。
# English: Test actual Han supplement decoding, external/embedded dual-mode loading,
#   coverage and bounded stack use. The external PIF4 (the distributed file under
#   flash/fonts/) exercises the file-loading path; the embedded PIF1 is the degraded
#   baseline used when the file is absent.
import struct,subprocess,tempfile,zlib,hashlib,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
asset=(ROOT/'main/assets'/'ui-hanzi.bin').read_bytes()
magic,count,px,block=struct.unpack_from('<4sIII',asset)
assert (magic,count,px,block)==(b'PIF1',5637,24,32)
# 内嵌 PIF1 基线逐字节 pin。/ Byte pin of the embedded PIF1 baseline.
assert hashlib.sha256(asset).hexdigest()=='6c1b29d0786b673a6ee8ced773cf48b3b08c837b453219f15ff9e01ee7f0155b'
distributed=sorted((ROOT/'flash'/'fonts').glob('ui-hanzi-*.bin'))
assert len(distributed)==1, 'exactly one distributed supplement expected'
full=distributed[0].read_bytes()
fmagic,fcount,fpx,fblock=struct.unpack_from('<4sIII',full)
assert (fmagic,fcount,fpx,fblock)==(b'PIF4',6030,24,32)
# 外置 PIF4 分发文件逐字节 pin。/ Byte pin of the distributed external PIF4 file.
assert hashlib.sha256(full).hexdigest()=='184eea2a747098250b389a589b8244430793a064f837c22a1550f3fca4dd571e'
def decode_all(data):
 cmagic,ccount,cpx,cblock=struct.unpack_from('<4sIII',data)
 wire=77 if cmagic==b'PIF1' else 293
 chars=struct.unpack_from('<'+'H'*ccount,data,16);assert list(chars)==sorted(set(chars))
 start=16+ccount*2;offsets=struct.unpack_from('<'+'I'*((ccount+31)//32+1),data,start);at=start+len(offsets)*4
 assert offsets[0]==0 and at+offsets[-1]==len(data)
 records=[]
 for i in range(len(offsets)-1):
  decoded=zlib.decompress(data[at+offsets[i]:at+offsets[i+1]])
  assert len(decoded)==min(32,ccount-i*32)*wire
  for off in range(0,len(decoded),wire):records.append(decoded[off:off+wire])
 return chars,records
def legacy_record(record,source_magic):
 if source_magic==b'PIF1':return record
 out=bytearray(record[:5]+bytes(72))
 for i in range(576):
  if ((record[5+i//2]>>((i%2)*4))&15)>=6:out[5+i//8]|=1<<(i%8)
 return bytes(out)
fchars,frecords=decode_all(full)
for record in frecords:assert 0<record[2]<=24 and 0<record[3]<=24 and 0<record[4]==17 and any(record[5:])
# 5637 个旧界面补字（除 reader 增补外）的像素 pin 不变。/ Legacy UI pixels pin.
extras_set=set()
extra_path=ROOT/'main/font/reader_extra_hanzi.txt'
if extra_path.exists():
 extras_set={int(line.split('#',1)[0].strip(),16) for line in extra_path.read_text().splitlines() if line.split('#',1)[0].strip()}
legacy=[legacy_record(r,fmagic) for cp,r in zip(fchars,frecords) if cp not in extras_set]
assert len(legacy)==5637 and hashlib.sha256(b''.join(legacy)).hexdigest()=='15e5824573335a02be0e90ea63f5ee5d3614831295dbfe80e2605b9c436afa8c', 'UI pixels/metrics changed'
pif1_chars,_=decode_all(asset)
extras=sorted(set(fchars)-set(pif1_chars));assert len(extras)==393
def run_variant(label,source,font_path):
 smagic,scount,spx,sblock=struct.unpack_from('<4sIII',source)
 wire=77 if smagic==b'PIF1' else 293
 schars,srecords=decode_all(source)
 slegacy=[legacy_record(r,smagic) for r in srecords]
 with tempfile.TemporaryDirectory() as temp:
  work=Path(temp)
  (work/'esp_heap_caps.h').write_text('''#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern int fail;
static inline void *heap_caps_malloc(size_t n,int c){(void)c;return fail?NULL:malloc(n);}
static inline void heap_caps_free(void *p){free(p);}
static inline void *heap_caps_calloc(size_t n,size_t s,int c){(void)c;return fail?NULL:calloc(n,s);}
''')
  (work/'miniz.h').write_text('''#pragma once
#include <stddef.h>
#include <zlib.h>
typedef struct {int state;} tinfl_decompressor;
typedef int tinfl_status;
#define TINFL_STATUS_DONE 0
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
#define tinfl_init(s) ((s)->state=0)
static inline tinfl_status tinfl_decompress(tinfl_decompressor *s,const unsigned char *in,size_t *n,unsigned char *base,unsigned char *out,size_t *m,int flags){(void)s;(void)base;(void)flags;uLongf size=*m;int result=uncompress(out,&size,in,*n);*m=size;return result==Z_OK?0:-1;}
''')
  assets='#include <stdint.h>\n'
  for filename,symbol in [('builtin.ttf','_binary_builtin_ttf'),('ui-hanzi.bin','_binary_ui_hanzi_bin')]:
   payload=(ROOT/'main/assets'/filename).read_bytes();var=filename.replace('.','_').replace('-','_')
   assets+=f'const uint8_t {var}[] asm("{symbol}_start")={{'+','.join(map(str,payload))+'};\n'
   assets+=f'asm(".globl {symbol}_end\\n.set {symbol}_end, {symbol}_start + {len(payload)}");\n'
  (work/'assets.c').write_text(assets)
  unit='''#include "ui_hanzi.h"
#include "ui_font.h"
#include <zlib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
int fail;
typedef struct {const uint16_t*cps;const uint32_t*hashes;size_t count;unsigned lane;ui_hanzi_cache_t cache;} job_t;
static void*decode_lane(void*arg){
 job_t*job=arg;uint8_t r[77];
 for(unsigned n=0;n<3000;++n){size_t i=(n*131u+job->lane*29u)%job->count;
  bool ok=job->lane?ui_hanzi_get_cached(&job->cache,job->cps[i],r):ui_hanzi_get(job->cps[i],r);
  assert(ok&&crc32(0,r,77)==job->hashes[i]);
 }
 ui_hanzi_cache_release(&job->cache);assert(!job->cache.block&&!job->cache.inflate&&!job->cache.font&&!job->cache.packed);return NULL;
}

static unsigned painted;
static bool capture;
static uint8_t actual[192*192];
void epd_draw_pixel(int x,int y,uint8_t color,uint8_t *fb){(void)fb;painted++;if(capture){assert(x>=0&&x<192&&y>=0&&y<192);assert(color==0);actual[y*192+x]=1;}}
int main(void){
 uint8_t record[77],fb=0;
 // 装载失败（分配耗尽）不崩溃，重试后成功。/ Load failure under OOM recovers on retry.
 fail=1;assert(!ui_hanzi_get(TEST_CP,record));fail=0;
 assert(ui_hanzi_get(TEST_CP,record));assert(!ui_hanzi_get(0x10ffff,record));
 static const uint16_t extra_cps[]={EXTRA_CPS};
 for(unsigned i=0;i<N_EXTRAS;++i)assert(ui_hanzi_has(extra_cps[i])==EXPECT_EXTRAS);
 static const uint16_t cps[]={CPS};
 for(unsigned i=0;i<sizeof(cps)/sizeof(cps[0]);i++){
  assert(ui_hanzi_has(cps[i])&&ui_hanzi_get(cps[i],record));
  char utf[4]={0xe0|(cps[i]>>12),0x80|((cps[i]>>6)&63),0x80|(cps[i]&63),0};assert(ui_font_has_text(utf));
  if(i%487==0)for(int px=18;px<=64;px+=7){int top,bottom;ui_font_measure_line_px(px,utf,&top,&bottom);assert(top>0&&bottom>=0);assert(ui_font_text_width_px(px,utf)>0);
   painted=0;ui_font_draw_text_px(&fb,100,100,px,utf,EPD_DRAW_ALIGN_LEFT,0,15,false);assert(painted>0);
   assert(ui_font_title_px(px,utf)==24);memset(actual,0,sizeof(actual));capture=true;
   ui_font_draw_title_px(&fb,100,100,px,utf,EPD_DRAW_ALIGN_LEFT);capture=false;
   for(int y=0;y<192;++y)for(int x=0;x<192;++x){int xx=x-100-(int8_t)record[0],yy=y-100-(int8_t)record[1];
    unsigned expected=0;if(xx>=0&&yy>=0&&xx<record[2]&&yy<record[3]){unsigned bit=yy*24+xx;expected=(record[5+bit/8]>>(bit&7))&1u;}
    assert(actual[y*192+x]==expected);}}
 }
 static const uint32_t hashes[]={HASHES};job_t jobs[3]={0};pthread_t threads[3];
 for(unsigned lane=0;lane<3;++lane){jobs[lane].cps=cps;jobs[lane].hashes=hashes;jobs[lane].count=sizeof(cps)/sizeof(cps[0]);jobs[lane].lane=lane;assert(!pthread_create(&threads[lane],NULL,decode_lane,&jobs[lane]));}
 for(unsigned lane=0;lane<3;++lane)assert(!pthread_join(threads[lane],NULL));
 assert(ui_font_title_px(48,"ABC")==48);
 puts("MODE result");
}
 '''.replace('TEST_CP',str(schars[0])).replace('N_EXTRAS',str(len(extras))).replace('EXTRA_CPS',','.join(map(str,extras))).replace('EXPECT_EXTRAS','1' if font_path else '0').replace('CPS',','.join(map(str,schars))).replace('HASHES',','.join(str(zlib.crc32(slegacy[i]))+'u' for i in range(scount))).replace('MODE result',f'Han supplement {label}: PASS')
  (work/'test.c').write_text(unit)
  flags=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-I'+str(work),'-I'+str(ROOT/'main/font'),'-I'+str(ROOT/'tools/ui_gesture_stubs')]
  if font_path:
   # Windows 命令行会剥掉 -D 值里的引号，改用强制包含头注入路径。/ -D quoting breaks on Windows; inject via a forced include.
   (work/'font_path.h').write_text('#define UI_HANZI_FLASH_FONT_PATH "%s"\n'%font_path)
   flags+=['-include',str(work/'font_path.h')]
  subprocess.run(flags+[str(work/'test.c'),str(work/'assets.c'),str(ROOT/'main/font/ui_font.c'),str(ROOT/'main/font/ui_hanzi.c'),'-lz','-o',str(work/'test')],check=True)
  subprocess.run([str(work/'test')],check=True)
# 降级分支：宿主上默认路径不存在，自动落到内嵌 PIF1。
# Degraded branch: the default path is absent on hosts, so the embedded PIF1 kicks in.
run_variant('embedded fallback',asset,None)
# 文件模式：把分发文件放进伪造的 /flash 路径。/ File mode with a forged /flash path.
with tempfile.TemporaryDirectory() as temp:
 staged=Path(temp)/'ui-hanzi.bin';staged.write_bytes(full)
 run_variant('external file',full,Path(staged).as_posix())
print(f'Compressed blocks: PASS (embedded {count} glyphs/{len(asset)} B, external {fcount} glyphs/{len(full)} B; bounded decoding)')
