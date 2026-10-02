// Offline native regression tests using production IR/cache code and an in-memory
// filesystem. --wrap=realloc injects real allocation failures under -fno-exceptions.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <utility>
#include <Esp.h>
#include <HalStorage.h>
#include <Serialization.h>
#include "ChapterIr.h"
#include "PageMap.h"
#include "LaidOutPage.h"
#include "HtmlToIr.h"

EspStub ESP;
static long failAfter = -1;
static size_t reallocCalls = 0;
extern "C" void* __real_realloc(void*, size_t);
extern "C" void* __wrap_realloc(void* p, size_t n) {
  ++reallocCalls;
  if (failAfter == 0) return nullptr;
  if (failAfter > 0) --failAfter;
  return __real_realloc(p, n);
}
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); return false; } } while (0)
using namespace rivulet;
static void begin(ChapterIr& ir) { ir.beginBlock(BlockKind::Paragraph, Align::Left, 0); }
static bool append(ChapterIr& ir, const std::string& s) { return ir.appendRun(RunStyle::Regular, SizeStep::Body, s); }
static std::string flatten(const ChapterIr& ir) {
  std::string text;
  for (const Run& r : ir.runs()) text += ir.runString(r);
  return text;
}
static std::vector<uint8_t> bytes(const char* path) {
  HalFile f; Storage.openFileForRead("TEST", path, f);
  std::vector<uint8_t> data(f.size());
  f.read(data.data(), data.size()); f.close(); return data;
}
static void store(const char* path, const std::vector<uint8_t>& data) {
  HalFile f; Storage.openFileForWrite("TEST", path, f); f.write(data.data(), data.size()); f.close();
}
template<typename T> static void put(std::vector<uint8_t>& data, size_t at, const T& value) {
  if (at + sizeof(T) > data.size()) std::abort();
  std::memcpy(data.data() + at, &value, sizeof(T));
}
static RenderKey key() { RenderKey k; k.fontId = -324599973; k.viewportW=440; k.viewportH=680; return k; }
static LaidOutPage page() {
  LaidOutPage p; p.start={0,0,0}; p.end={1,1,0}; p.contentH=24;
  GlyphSpan sp; sp.fontId=key().fontId; sp.text="A readable page";
  p.spans.push_back(std::move(sp)); return p;
}
static bool checked_vector_oom_preserves_data() {
  CheckedVector<Run> v; Run r; r.textOff=72; CHECK(v.push_back(r));
  const auto cap=v.capacity(); failAfter=0;
  CHECK(!v.reserve(cap+100)); CHECK(v.size()==1); CHECK(v[0].textOff==72);
  CHECK(!v.resize(cap+100)); CHECK(v.size()==1);
  failAfter=-1; CHECK(v.push_back(v[0])); CHECK(v.back().textOff==72);
  return true;
}
static bool checked_vector_overflow_rejected() {
  CheckedVector<Run> v;
  CHECK(!v.reserve(std::numeric_limits<size_t>::max())); CHECK(v.empty()); CHECK(v.capacity()==0);
  return true;
}
static bool checked_vector_move_and_release() {
  CheckedVector<Run> a; CHECK(a.resize(12)); a[4].textOff=9;
  CheckedVector<Run> b(std::move(a)); CHECK(a.empty()); CHECK(b[4].textOff==9);
  b.release(); CHECK(b.capacity()==0); CHECK(b.data()==nullptr); return true;
}
static bool long_run_keeps_every_byte() {
  ChapterIr ir; begin(ir); const std::string text(140*1024,'a');
  CHECK(append(ir,text)); ir.endBlock(); CHECK(!ir.failed());
  CHECK(ir.runs().size()==3); CHECK(flatten(ir)==text); CHECK(ir.blocks()[0].runCount==3);
  CHECK(ir.saveToFile("/ir")); ChapterIr copy; CHECK(copy.loadFromFile("/ir")); CHECK(flatten(copy)==text);
  return true;
}
static bool coalescing_past_64k_keeps_tail() {
  ChapterIr ir; begin(ir); std::string expected;
  for (int i=0;i<180;++i) { std::string text(700, char('a'+i%26)); expected+=text; CHECK(append(ir,text)); }
  ir.endBlock(); CHECK(flatten(ir)==expected); CHECK(ir.runs().size()==2); return true;
}
static bool unicode_not_split_at_run_boundary() {
  ChapterIr ir; begin(ir); std::string text(65534,'x'); text += "\xF0\x9F\x93\x96"; text += std::string(800,'y');
  CHECK(append(ir,text)); ir.endBlock(); CHECK(flatten(ir)==text);
  CHECK(ir.runs()[0].textLen==65534); CHECK(ir.runString(ir.runs()[1]).substr(0,4)=="\xF0\x9F\x93\x96");
  return true;
}
static bool unicode_coalesce_short_room() {
  ChapterIr ir; begin(ir); CHECK(append(ir,std::string(65533,'a')));
  CHECK(append(ir,"\xF0\x9F\x93\x96 tail")); ir.endBlock();
  CHECK(ir.runs()[0].textLen==65533); CHECK(ir.runs()[1].textLen==9); return true;
}
static bool full_blob_allows_inplace_replacement() {
  ChapterIr ir; begin(ir); CHECK(append(ir,std::string(192*1024,'a'))); ir.endBlock();
  std::string replacement(ir.runs()[0].textLen,'z'); failAfter=0;
  CHECK(ir.setRunText(0,replacement)); CHECK(ir.textSize()==192*1024);
  CHECK(ir.runString(ir.runs()[0])==replacement); return true;
}
static bool append_size_overflow_rejected() {
  ChapterIr ir; begin(ir); CHECK(!ir.appendRun(RunStyle::Regular,SizeStep::Body,"x",SIZE_MAX));
  CHECK(ir.failed()); CHECK(ir.textSize()==0); return true;
}
static bool corrupt_run_offset_is_safe() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"valid text")); ir.endBlock();
  Run bad; bad.textOff=UINT32_MAX; bad.textLen=20;
  CHECK(std::strcmp(ir.runText(bad),"")==0); CHECK(ir.runString(bad).empty()); return true;
}
static bool replacement_refuses_16bit_truncation() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"original")); ir.endBlock();
  CHECK(!ir.setRunText(0,std::string(65536,'x'))); CHECK(flatten(ir)=="original"); return true;
}
static bool alias_append_survives_reallocation() {
  ChapterIr ir; begin(ir); std::string text(4000,'a'); CHECK(append(ir,text));
  const char* src=ir.textData(); CHECK(ir.appendRun(RunStyle::Bold,SizeStep::Body,src,4000));
  ir.endBlock(); CHECK(flatten(ir)==text+text); return true;
}
static bool alias_replacement_survives_reallocation() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"abc")); CHECK(ir.appendRun(RunStyle::Bold,SizeStep::Body,std::string(4000,'z')));
  const char* src=ir.textData()+3; CHECK(ir.setRunText(0,src,4000)); ir.endBlock();
  CHECK(ir.runString(ir.runs()[0])==std::string(4000,'z')); return true;
}
static bool alias_past_blob_rejected() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"abc"));
  CHECK(!ir.appendRun(RunStyle::Regular,SizeStep::Body,ir.textData()+2,8)); return true;
}
static bool chapter_append_oom_returns_failure() {
  ChapterIr ir; begin(ir); failAfter=0;
  CHECK(!append(ir,"not allocated")); CHECK(ir.failed()); CHECK(ir.runs().empty()); CHECK(ir.textSize()==0); return true;
}
static bool chapter_clear_releases_metadata() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"text")); ir.endBlock();
  CHECK(ir.blocks().capacity()>0); ir.clear();
  CHECK(ir.blocks().capacity()==0); CHECK(ir.runs().capacity()==0); CHECK(ir.textSize()==0); return true;
}
static bool corrupt_ir_is_rejected_before_allocation() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"text")); ir.endBlock(); CHECK(ir.saveToFile("/ir"));
  auto data=bytes("/ir"); put(data,6,uint32_t{4096}); store("/bad",data);
  reallocCalls=0; ChapterIr bad; CHECK(!bad.loadFromFile("/bad")); CHECK(reallocCalls==0); return true;
}
static bool corrupt_ir_run_bounds_rejected() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"text")); ir.endBlock(); CHECK(ir.saveToFile("/ir"));
  auto data=bytes("/ir"); put(data,33,uint32_t{UINT32_MAX}); store("/bad",data);
  ChapterIr bad; CHECK(!bad.loadFromFile("/bad")); CHECK(bad.empty()); return true;
}
static bool corrupt_ir_block_range_rejected() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"text")); ir.endBlock(); CHECK(ir.saveToFile("/ir"));
  auto data=bytes("/ir"); put(data,18+9,uint16_t{65535}); store("/bad",data);
  ChapterIr bad; CHECK(!bad.loadFromFile("/bad")); return true;
}
static bool ir_loader_oom_returns_failure() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"text")); ir.endBlock(); CHECK(ir.saveToFile("/ir"));
  failAfter=0; ChapterIr copy; CHECK(!copy.loadFromFile("/ir")); CHECK(copy.empty()); return true;
}
static bool failed_ir_is_not_persisted() {
  ChapterIr ir; begin(ir); CHECK(append(ir,"partial")); ir.endBlock(); ir.markFailed();
  CHECK(!ir.saveToFile("/partial")); CHECK(!Storage.exists("/partial")); return true;
}
static bool render_key_covers_spacing_and_images() {
  auto a=key(), b=a; b.pad=0x20; CHECK(a!=b); b=a; b.pad=2; CHECK(a!=b); b=a; CHECK(a==b); return true;
}
static bool bounded_string_rejects_large_payload_before_resize() {
  HalFile f; CHECK(Storage.openFileForWrite("TEST","/str",f));
  CHECK(serialization::tryWriteString(f,std::string(20000,'x'))); f.close();
  CHECK(Storage.openFileForRead("TEST","/str",f)); std::string out="unchanged";
  CHECK(!serialization::tryReadString(f,out,4096)); CHECK(out=="unchanged"); return true;
}
static bool valid_page_cache_roundtrip() {
  auto p=page(); CHECK(p.saveToFile("/page",key(),0)); LaidOutPage q;
  CHECK(q.loadFromFile("/page",key(),0)); CHECK(q.spans.size()==1); CHECK(q.spans[0].text==p.spans[0].text); return true;
}
static bool page_cache_large_string_rejected() {
  auto p=page(); p.spans[0].text=std::string(5000,'x'); CHECK(p.saveToFile("/page",key(),0));
  LaidOutPage q; CHECK(!q.loadFromFile("/page",key(),0)); CHECK(q.spans.empty()); return true;
}
static bool page_cache_long_href_rejected() {
  LaidOutPage p; ImagePlate im; im.href=std::string(2000,'x'); p.images.push_back(std::move(im));
  CHECK(p.saveToFile("/page",key(),0)); LaidOutPage q;
  CHECK(!q.loadFromFile("/page",key(),0)); CHECK(q.images.empty()); return true;
}
static bool page_cache_fake_span_count_rejected() {
  auto p=page(); CHECK(p.saveToFile("/page",key(),0)); auto data=bytes("/page");
  put(data,4+2+sizeof(RenderKey)+4+12+8,uint32_t{2000}); store("/bad",data);
  LaidOutPage q; CHECK(!q.loadFromFile("/bad",key(),0)); return true;
}
static bool page_cache_spacing_mismatch_rejected() {
  auto p=page(); CHECK(p.saveToFile("/page",key(),0)); auto other=key(); other.pad=0x20;
  LaidOutPage q; CHECK(!q.loadFromFile("/page",other,0)); return true;
}
static bool page_cache_low_memory_bypasses_cache() {
  auto p=page(); CHECK(p.saveToFile("/page",key(),0)); ESP.setFreeHeap(12000);
  LaidOutPage q; CHECK(!q.loadFromFile("/page",key(),0)); return true;
}
static bool cache_long_paths_do_not_overwrite_live_file() {
  std::string path(245,'x'); auto p=page(); CHECK(!p.saveToFile(path.c_str(),key(),0));
  CHECK(!Storage.exists(path.c_str())); PageMap map; CHECK(map.resetWithStart({0,0,0}));
  CHECK(!map.saveToFile(path.c_str())); CHECK(!Storage.exists(path.c_str())); return true;
}
static bool page_map_oom_preserves_existing_starts() {
  PageMap map; CHECK(map.resetWithStart({0,0,0}));
  for(uint16_t i=1;i<8;++i) CHECK(map.pushPageStart({0,0,i}));
  failAfter=0; CHECK(!map.pushPageStart({0,0,8})); CHECK(map.knownPages()==8);
  CHECK(map.pageStart(7).byteInRun==7); CHECK(!map.complete()); return true;
}
static bool page_map_live_cap_is_enforced() {
  PageMap map; CHECK(map.resetWithStart({0,0,0}));
  for(uint16_t i=1;i<4000;++i) CHECK(map.pushPageStart({0,0,i}));
  CHECK(!map.pushPageStart({0,0,4000})); CHECK(map.knownPages()==4000); CHECK(!map.complete()); return true;
}
static bool page_map_roundtrip_and_bad_order() {
  PageMap map; map.setRenderKey(key()); CHECK(map.resetWithStart({0,0,0})); CHECK(map.pushPageStart({0,0,10}));
  map.markComplete(2); CHECK(map.saveToFile("/map")); PageMap copy;
  CHECK(copy.loadFromFile("/map")); CHECK(copy.complete()); CHECK(copy.knownTotal()==2);
  auto data=bytes("/map"); size_t at=4+2+sizeof(RenderKey)+4+1+4;
  put(data,at+6+4,uint16_t{0}); store("/bad",data); CHECK(!copy.loadFromFile("/bad")); CHECK(copy.empty()); return true;
}
static bool page_map_inconsistent_total_rejected() {
  PageMap map; map.setRenderKey(key()); CHECK(map.resetWithStart({0,0,0})); map.markComplete(1);
  CHECK(map.saveToFile("/map")); auto data=bytes("/map"); put(data,4+2+sizeof(RenderKey)+4+1,int{700}); store("/bad",data);
  PageMap q; CHECK(!q.loadFromFile("/bad")); return true;
}
static bool page_map_loader_oom_returns_failure() {
  PageMap map; map.setRenderKey(key()); CHECK(map.resetWithStart({0,0,0})); CHECK(map.saveToFile("/map"));
  failAfter=0; PageMap q; CHECK(!q.loadFromFile("/map")); CHECK(q.empty()); return true;
}
static bool html_large_paragraph_roundtrip() {
  std::string text; for(int i=0;i<16000;++i) text+="word ";
  const auto html="<html><body><p>"+text+"</p></body></html>";
  ChapterIr ir; CHECK(HtmlToIr::convert(html.data(),html.size(),ir)); CHECK(!ir.failed());
  const auto flattened=flatten(ir); CHECK(flattened.size()>=text.size()-1); CHECK(flattened.find("word word")==0); return true;
}
int main() {
  struct Test { const char* name; bool (*run)(); };
#define T(fn) {#fn,fn}
  const Test tests[]={
    T(checked_vector_oom_preserves_data),T(checked_vector_overflow_rejected),T(checked_vector_move_and_release),
    T(long_run_keeps_every_byte),T(coalescing_past_64k_keeps_tail),T(unicode_not_split_at_run_boundary),
    T(unicode_coalesce_short_room),T(full_blob_allows_inplace_replacement),T(append_size_overflow_rejected),
    T(corrupt_run_offset_is_safe),T(replacement_refuses_16bit_truncation),T(alias_append_survives_reallocation),
    T(alias_replacement_survives_reallocation),T(alias_past_blob_rejected),T(chapter_append_oom_returns_failure),
    T(chapter_clear_releases_metadata),T(corrupt_ir_is_rejected_before_allocation),T(corrupt_ir_run_bounds_rejected),
    T(corrupt_ir_block_range_rejected),T(ir_loader_oom_returns_failure),T(failed_ir_is_not_persisted),
    T(render_key_covers_spacing_and_images),T(bounded_string_rejects_large_payload_before_resize),
    T(valid_page_cache_roundtrip),T(page_cache_large_string_rejected),T(page_cache_long_href_rejected),
    T(page_cache_fake_span_count_rejected),T(page_cache_spacing_mismatch_rejected),T(page_cache_low_memory_bypasses_cache),
    T(cache_long_paths_do_not_overwrite_live_file),T(page_map_oom_preserves_existing_starts),T(page_map_live_cap_is_enforced),
    T(page_map_roundtrip_and_bad_order),T(page_map_inconsistent_total_rejected),T(page_map_loader_oom_returns_failure),
    T(html_large_paragraph_roundtrip)
  };
  int failed=0;
  for(const auto& test:tests) {
    failAfter=-1; reallocCalls=0; ESP.reset(); Storage.reset();
    const bool ok=test.run(); failed+=!ok;
    std::printf("%s %s\n",ok?"PASS":"FAIL",test.name);
  }
  std::printf("%zu tests, %d failures\n",std::size(tests),failed);
  return failed?1:0;
}
