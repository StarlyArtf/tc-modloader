#include "../../sdk/tc_ui_texture.h"
#include <algorithm>

using namespace tc::ui;
static bool show = false;
static bool grid = true;
static float thickness = 3;
static Vec2 control{180, 120};
static int clicks = 0;
static Texture picture, generated;
static int textureStatus=-1;
static bool flipImage=false;

static void reloadImages() {
    textureStatus=picture.load("native/images/checker.png",TextureFilter::Nearest);
    const unsigned char pixels[]{255,80,100,255, 70,175,235,255,
                                 100,220,165,128, 255,205,80,255};
    const int rgbaStatus=generated.createRgba(2,2,pixels,sizeof(pixels),TextureFilter::Nearest);
    if(!textureStatus)textureStatus=rgbaStatus;
}

static void draw(void*, const TCFrame*, float, float) {
    text("Drag inside the canvas to move the curve control point.");
    checkbox("Grid", &grid);
    sameLine();
    sliderFloat("Stroke", &thickness, 1, 10);
    if(texturesReady()) {
        checkbox("Flip image", &flipImage); sameLine();
        if(button("Reload images"))reloadImages();
        sameLine();
        if(button("Release images")){picture.reset();generated.reset();}
        if(textureStatus)text("Image load status: "+std::to_string(textureStatus));
    } else textDisabled("Images require a loader with the texture capability.");
    const Vec2 available = contentAvailable();
    {
        Canvas canvas("drawing", {std::max(80.f, available.x), 360});
        if (canvas) {
            const Vec2 size = canvas.size();
            canvas.rectFilled({0,0}, size, rgba(28,31,40), 8);
            if (grid) {
                for (float x=0; x<size.x; x+=24) canvas.line({x,0},{x,size.y},rgba(48,53,64));
                for (float y=0; y<size.y; y+=24) canvas.line({0,y},{size.x,y},rgba(48,53,64));
            }
            if (canvas.dragging()) {
                const Vec2 mouse = canvas.mousePosition();
                control = {std::clamp(mouse.x, 0.f, size.x), std::clamp(mouse.y, 0.f, size.y)};
            }
            if (canvas.clicked()) ++clicks;
            canvas.rect({20,20},{120,85},rgba(245,172,65),12,thickness);
            canvas.circleFilled({170,55},28,rgba(76,153,220));
            canvas.circle({170,55},34,rgba(167,211,247),thickness);
            canvas.triangleFilled({240,20},{280,85},{205,85},rgba(92,195,154));
            canvas.gradient({310,20},{440,85},rgba(238,88,92),rgba(245,184,77),
                            rgba(92,121,224),rgba(160,101,207));
            canvas.line({30,240},control,rgba(100,120,145));
            canvas.bezier({30,240},control,{340,310},{440,170},rgba(240,187,63),thickness);
            canvas.circleFilled(control,7,rgba(255,231,174));
            const Vec2 polygon[]{{480,30},{570,30},{525,65},{570,110},{480,110}};
            canvas.concaveFilled(polygon,5,rgba(155,112,224));
            const Vec2 wave[]{{25,300},{90,280},{140,320},{210,290},{270,310}};
            canvas.polyline(wave,5,rgba(86,200,180),false,thickness);
            {
                Canvas::Clip clip(canvas,{310,270},{440,330});
                canvas.circleFilled({310,300},65,rgba(229,106,138));
                canvas.text({320,280},rgba(255,255,255),"Clipped");
            }
            canvas.text({22,100},rgba(215,221,232),"Shapes / Bezier / polygons / clipping");
            picture.draw(canvas,{480,150},{600,230},flipImage?Vec2{1,0}:Vec2{0,0},
                         flipImage?Vec2{0,1}:Vec2{1,1});
            generated.draw(canvas,{480,250},{560,330});
        }
    } // restore clip BEFORE submitting ordinary widgets
    text("Canvas clicks: " + std::to_string(clicks));
    if (button("Reset drawing")) { control = {180,120}; clicks = 0; }
}

static void frame(void*, const TCFrame* frameInfo) {
    toggleHotkey(VK_F8, &show);
    if (!show) return;
    if (auto window = panel("Custom drawing###DrawingDemo", &show, {680,550}, {100,120}))
        draw(nullptr, frameInfo, 0, 0);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    if (!host || host->api_version != TC_MOD_API_VERSION || host->size < TC_HOST_BASE_SIZE ||
        !plugin || plugin->size < sizeof(TCPlugin)) return 1;
    if (!load(host) || !loadDrawing(host)) return 2;
    if(loadTextures(host))reloadImages();
    const int page = registerPage("canvas", "Custom drawing", draw, nullptr, host);
    if (page != 0 && page != -1) return 3;
    plugin->on_frame = frame;
    return 0;
}
