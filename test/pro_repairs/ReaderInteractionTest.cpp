#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <Esp.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <ReleaseJsonParser.h>
#include "RivuletEngine.h"
#include "FontLadder.h"
#include "Epub/css/StyleResolve.h"
#include "util/LeftBrightnessGesture.h"
#include "activities/reader/BookAppearanceGeometry.h"
EspStub ESP;
static long failAfter = -1;
extern "C" void* __real_realloc(void*, size_t);
extern "C" void* __wrap_realloc(void* p, size_t n) {
  if (failAfter == 0) return nullptr;
  if (failAfter > 0) --failAfter;
  return __real_realloc(p,n);
}
#define CHECK(e) do { if(!(e)){std::fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#e);return false;} } while(0)
using namespace rivulet;
static RenderKey key(int id=-324599973) { RenderKey k; k.fontId=id;k.viewportW=180;k.viewportH=160;return k; }
static std::string book() {std::string s; for(int i=0;i<80;++i)s += "<p>Alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima.</p>"; return s;}
static bool setup(RivuletEngine& e, GfxRenderer& g, int page=0) {
  e.setRenderKey(key()); const auto b=book();
  return e.ingestHtml(b.data(),b.size(),nullptr) && e.goToPage(g,page,200);
}
static bool brightness_hold_does_not_jump() {
  LeftBrightnessGesture g; auto r=g.update(true,480,800,true,10,true,650,37);
  CHECK(r.consumed&&!r.changed&&!r.save&&r.level==37);r=g.update(true,480,800,false,0,true,645,37);
  CHECK(!r.changed&&r.level==37); r=g.update(true,480,800,false,0,false,0,37);
  CHECK(r.consumed&&!r.save&&!g.active()); return true;
}
static bool brightness_relative_drag_and_release() {
  LeftBrightnessGesture g; g.update(true,480,800,true,10,true,650,40);
  auto r=g.update(true,480,800,false,0,true,470,40); CHECK(r.changed&&r.level==90);
  r=g.update(true,480,800,false,0,true,450,90);CHECK(r.consumed);
  r=g.update(true,480,800,false,0,false,0,95);CHECK(r.consumed&&r.save&&!g.active());
  r=g.update(true,480,800,false,0,false,0,95);CHECK(!r.consumed&&!r.save);return true;
}
static bool brightness_bounds_and_cancel() {
  LeftBrightnessGesture g; CHECK(!g.update(true,480,800,true,240,true,400,40).consumed);
  g.update(true,480,800,true,1,true,700,40);CHECK(g.update(true,480,800,false,0,true,0,40).level==100);
  CHECK(g.update(true,480,800,false,0,true,799,100).level==13);
  const auto r=g.update(false,480,800,false,0,true,799,13);CHECK(r.consumed&&r.save&&!g.active());return true;
}
static bool bottom_left_swipe_does_not_arm_brightness() {
  LeftBrightnessGesture g;CHECK(!g.update(true,480,800,false,2,true,790,40).consumed);
  CHECK(!g.update(true,480,800,false,2,true,600,40).consumed);
  CHECK(!g.update(true,480,800,false,2,false,600,40).consumed);return true;
}
static bool drawer_geometry_portrait_landscape() {
  for (auto wh : {std::pair<int,int>{480,800},{800,480},{480,648}}) {
    auto g=BookAppearanceGeometry::make(wh.first,wh.second);
    CHECK(g.top>0&&g.footer+32<=g.height&&g.rowHeight>=38);
    CHECK(g.rowAt(g.rowsTop)==0&&g.rowAt(g.footer-1)==2&&g.rowAt(g.footer)==-1);
    for(int i=0;i<4;++i)CHECK(g.tabAt(i*g.width/4+1,g.top+g.header+1)==i);
    CHECK(g.tabAt(g.width,g.top+g.header+1)==-1&&g.tabAt(0,g.top)==-1);
  }return true;
}
static bool literata_alias_uses_real_rung() {
  CHECK(FontLadder::resolve(-1128177077,static_cast<SizeStep>(3))==2090520927);
  CHECK(FontLadder::resolve(-209681255,static_cast<SizeStep>(1))==-847079762);
  GfxRenderer g;StyleResolveContext c;initStyleResolveContext(c,-1128177077,1,false,g);
  CHECK(resolveRelativeFontId(c,3)==2090520927);return true;
}
static bool sd_ladder_supports_negative_ids() {
  GfxRenderer g;setStyleLadderFillHook([](void*,int,int out[5]){for(int i=0;i<5;++i)out[i]=-9000-i;return true;},nullptr);
  StyleResolveContext c;initStyleResolveContext(c,-9002,1,false,g);
  CHECK(!c.singleSizeFamily&&resolveRelativeFontId(c,2)==-9002&&resolveRelativeFontId(c,4)==-9004);
  setStyleLadderFillHook(nullptr,nullptr);return true;
}
static bool measure_paint_same_cursor() {
  GfxRenderer g;RivuletEngine e;CHECK(setup(e,g));LayoutParams p;p.key=key();p.bodyEmPx=12;
  LaidOutPage a,b; CHECK(PageLayouter::layoutPage(e.chapter(),g,p,{},a));p.measureOnly=true;
  CHECK(PageLayouter::layoutPage(e.chapter(),g,p,{},b));CHECK(a.end==b.end&&!a.spans.empty()&&b.spans.empty());return true;
}
static bool reflow_preserves_passage() {
  GfxRenderer g;RivuletEngine e;CHECK(setup(e,g,12));const auto anchor=e.page().start;
  CHECK(e.reflowToCursor(g,key(-9002),1,anchor,300,0));
  CHECK(!(anchor<e.page().start)&&(anchor<e.page().end||e.page().atChapterEnd));
  CHECK(e.renderKey().fontId==-9002&&e.currentPage()!=12);return true;
}
static bool reflow_budget_rolls_back() {
  GfxRenderer g;RivuletEngine e;CHECK(setup(e,g,12));const auto start=e.page().start,end=e.page().end;
  const auto text=e.page().spans[0].text;const int page=e.currentPage();
  CHECK(!e.reflowToCursor(g,key(-9002),1,start,1,0));
  CHECK(e.page().start==start&&e.page().end==end&&e.currentPage()==page&&e.renderKey()==key());
  CHECK(e.page().spans[0].text==text);return true;
}
static bool reflow_oom_rolls_back() {
  GfxRenderer g;RivuletEngine e;CHECK(setup(e,g,12));const auto start=e.page().start,end=e.page().end;
  failAfter=0;CHECK(!e.reflowToCursor(g,key(-9002),1,start,300,0));failAfter=-1;
  CHECK(e.page().start==start&&e.page().end==end&&e.renderKey()==key());CHECK(e.nextPage(g));return true;
}
static bool tokenizer_oom_is_not_end_or_skipped_block() {
  GfxRenderer g;RivuletEngine e;CHECK(setup(e,g));LayoutParams p;p.key=key();p.measureOnly=true;LaidOutPage a;
  failAfter=0;CHECK(!PageLayouter::layoutPage(e.chapter(),g,p,{},a));failAfter=-1;
  CHECK(a.storageFailed&&!a.atChapterEnd);CHECK(!a.saveToFile("/partial-page",key(),0));return true;
}
static bool html_prefix_not_cached_as_complete() {
  RivuletEngine e;auto b=std::string("<p>")+std::string(180*1024,'x')+"</p>";
  e.ingestHtml(b.data(),b.size(),"/oversize-ir");CHECK(!Storage.exists("/oversize-ir"));CHECK(e.chapter().failed());return true;
}
static std::string asset(const std::string& name,const std::string& size="100000",const std::string& url="https://example.org/app.bin") {
  return "{\"tag_name\":\"v0.2.0\",\"assets\":[{\"name\":\""+name+"\",\"size\":"+size+",\"browser_download_url\":\""+url+"\"}]}";
}
static bool parsePro(const std::string& j) {ReleaseJsonParser p(ReleaseJsonParser::FirmwareTarget::X4Pro);for(char c:j)p.feed(&c,1);return p.foundTag()&&p.foundFirmware();}
static bool ota_requires_pro_application_asset() {
  CHECK(parsePro(asset("firmware-x4pro.bin")));CHECK(parsePro(asset("Casper-Pro-v0.2.0-rc1-x4pro.bin")));
  for(const char* n:{"firmware.bin","Casper-v0.2.0.bin","firmware-sticky.bin","merged-x4pro.bin","bootloader-x4pro.bin","Casper-Pro-v0.2.0-x4pro.bin.zip"})CHECK(!parsePro(asset(n)));
  return true;
}
static bool ota_rejects_overflow_truncation_empty() {
  CHECK(!parsePro(asset("firmware-x4pro.bin","99999999999999999999999999999")));
  CHECK(!parsePro(asset("firmware-x4pro.bin","-1")));CHECK(!parsePro(asset("firmware-x4pro.bin","0")));
  CHECK(!parsePro(asset("firmware-x4pro.bin","100000","")));
  CHECK(!parsePro(asset("firmware-x4pro.bin","100000","http://example.org/app.bin")));
  CHECK(!parsePro(asset(std::string(120,'a')+"-x4pro.bin")));
  CHECK(!parsePro(asset("firmware-x4pro.bin","100000","https://example.org/"+std::string(600,'x'))));return true;
}
int main(){
  struct T{const char* name;bool(*fn)();};
#define TST(f) T{#f,f}
  const T tests[]={TST(brightness_hold_does_not_jump),TST(brightness_relative_drag_and_release),TST(brightness_bounds_and_cancel),TST(bottom_left_swipe_does_not_arm_brightness),TST(drawer_geometry_portrait_landscape),TST(literata_alias_uses_real_rung),TST(sd_ladder_supports_negative_ids),TST(measure_paint_same_cursor),TST(reflow_preserves_passage),TST(reflow_budget_rolls_back),TST(reflow_oom_rolls_back),TST(tokenizer_oom_is_not_end_or_skipped_block),TST(html_prefix_not_cached_as_complete),TST(ota_requires_pro_application_asset),TST(ota_rejects_overflow_truncation_empty)};
  int failed=0;for(const auto&t:tests){failAfter=-1;ESP.reset();Storage.reset();fakeMillis=0;setStyleLadderFillHook(nullptr,nullptr);const bool ok=t.fn();std::printf("%s %s\n",ok?"PASS":"FAIL",t.name);failed+=!ok;}
  std::printf("%zu tests, %d failures\n",sizeof(tests)/sizeof(*tests),failed);return failed?1:0;
}
