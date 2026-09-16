// Developer-only automation; never enabled by build-wire-palette.ps1.
static bool (*testInvisibleOriginal)(const char*,V2,int);static double testTime;static int testStage=0;static int testFrame=-1,testButtonIndex=0;
static bool testInvisible(const char*id,V2 s,int flags){bool result=testInvisibleOriginal(id,s,flags);auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);if(rva>=0x449df0&&rva<0x44b610){int n=api<int(*)()>("igGetFrameCount")();if(n!=testFrame){testFrame=n;testButtonIndex=0;}++testButtonIndex;if(testStage==0&&testTime>4&&testButtonIndex==2){testStage=1;host->log(host->context,(std::string("PLAYTEST activate home button ")+id).c_str());return true;}}return result;}
static void testCapture(){GLint v[4];glGetIntegerv(GL_VIEWPORT,v);int w=v[2],h=v[3];if(w<=0||h<=0)return;int stride=(w*3+3)&~3;std::vector<char> b(stride*h);GLint old,pack;glGetIntegerv(GL_READ_BUFFER,&old);glGetIntegerv(GL_PACK_ALIGNMENT,&pack);glReadBuffer(GL_FRONT);glPixelStorei(GL_PACK_ALIGNMENT,4);glReadPixels(0,0,w,h,GL_BGR,GL_UNSIGNED_BYTE,b.data());glReadBuffer(old);glPixelStorei(GL_PACK_ALIGNMENT,pack);BITMAPFILEHEADER f{};f.bfType=0x4d42;f.bfOffBits=54;f.bfSize=54+b.size();BITMAPINFOHEADER i{};i.biSize=40;i.biWidth=w;i.biHeight=h;i.biPlanes=1;i.biBitCount=24;std::ofstream out(file.parent_path()/"playtest.bmp",std::ios::binary);out.write((char*)&f,sizeof(f));out.write((char*)&i,sizeof(i));out.write(b.data(),b.size());}
static void testTick(const TCFrame*f){static double start=f->time_seconds;testTime=f->time_seconds-start;static bool committed=false;if(testTime>2&&!committed){commit();committed=true;host->log(host->context,"PLAYTEST committed RGB");}static bool recolored=false;if(testModel&&!recolored&&testTime>8){auto m=(unsigned char*)testModel;auto count=*(int64_t*)(m+0x98);auto payload=*(unsigned char**)(m+0xa0);if(count>0&&payload){auto start=sym<uint32_t(*)(void*)>("get_start__modelZsave95mongerZcommon_u4863");uint32_t point=start(payload+0x20);auto before=pipette(testModel,point);sym<void(*)(void*,uint32_t,uint8_t,bool)>("color_wire_point__modelZutilities_u282")(testModel,point,11,false);auto after=pipette(testModel,point);host->log(host->context,("PLAYTEST recolor + pipette: "+std::to_string(before)+" -> "+std::to_string(after)+" point="+std::to_string(point)).c_str());for(int n=0;n<6;++n){edit[0]=float(n+1)/10;edit[1]=0.7f;edit[2]=0.2f;commit();}bool evicted=std::find(palette.recent.begin(),palette.recent.end(),11)==palette.recent.end();picking=true;testClick=true;bool consumed=update(testModel,context,nullptr,point,0);bool ok=evicted&&consumed&&selected==11&&!picking&&pipette(testModel,point)==11;host->log(host->context,ok?"PLAYTEST PASS eyedropper consumes click and restores evicted RGB without recoloring":"PLAYTEST FAIL eyedropper");recolored=true;}}static int next=10;if(testTime>next){next+=10;testCapture();host->log(host->context,("PLAYTEST frame, board seen="+std::to_string(f->frame_number-seen)).c_str());}}


// Requires the isolated sandbox fixture used by wire-palette-playtest.hpp.
// Exercises actual ImGui input and checks framebuffer pixels, not uniform readback.
#ifdef TC_WIRE_RENDER_SELFTEST
static void testRenderTick(){
 static int stage=0;GLint vp[4];glGetIntegerv(GL_VIEWPORT,vp);int w=vp[2],h=vp[3];
 auto io=api<void*(*)()>("igGetIO")();
 if(testTime>15&&stage==0){edit[0]=1;edit[1]=0.2f;edit[2]=0.6f;commit();stage=1;}
 if(testTime>16&&testTime<27)api<void(*)(void*,float,float)>("ImGuiIO_AddMousePosEvent")(io,w*(testTime<19?0.72265625f:0.83984375f),h*0.757576f);
 if(testTime>18&&stage==1){api<void(*)(void*,int,bool)>("ImGuiIO_AddMouseButtonEvent")(io,0,true);stage=2;}
 auto check=[&](const char* label){
  GLint old,pack;glGetIntegerv(GL_READ_BUFFER,&old);glGetIntegerv(GL_PACK_ALIGNMENT,&pack);
  glReadBuffer(GL_FRONT);glPixelStorei(GL_PACK_ALIGNMENT,1);
  int x=int(w*0.55f),y=int(h*0.1f),rw=int(w*0.4f),rh=int(h*0.25f);
  std::vector<unsigned char> pixels(rw*rh*3);glReadPixels(x,y,rw,rh,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
  glReadBuffer(old);glPixelStorei(GL_PACK_ALIGNMENT,pack);int pink=0;
  for(size_t i=0;i<pixels.size();i+=3)if(pixels[i]>100&&pixels[i+1]<80&&pixels[i+2]>60&&pixels[i+2]<pixels[i])++pink;
  host->log(host->context,(std::string(pink>50?"RENDER PASS ":"RENDER FAIL ")+label+" pink pixels="+std::to_string(pink)).c_str());
  testCapture();std::filesystem::copy_file(file.parent_path()/"playtest.bmp",file.parent_path()/(std::string(label)+".bmp"),std::filesystem::copy_options::overwrite_existing);
 };
 if(testTime>21&&stage==2){check("drag-preview");stage=3;}
 if(testTime>23&&stage==3){api<void(*)(void*,int,bool)>("ImGuiIO_AddMouseButtonEvent")(io,0,false);stage=4;}
 if(testTime>27&&stage==4){check("placed-wire");edit[0]=0.1f;edit[1]=0.9f;edit[2]=0.2f;commit();stage=5;}
 if(testTime>31&&stage==5){check("old-wire-after-color-change");stage=6;}
}
#endif
