/*
 * wintv-capture (Windows): the DirectShow backend for the TV tuner helper, for the
 * Hauppauge WinTV USB tuners (HVR-950Q) and other WDM analog capture devices.
 * Everything else (captions, recording, the --serve protocol for HauppaugeRecorder,
 * the command line) is in tvcore.hpp; see there and docs/TVHELPER.md.
 *
 * The graph is: capture pin -> sample grabber (YUY2 video), the driver's raw VBI pin ->
 * sample grabber, and the USB audio device -> sample grabber (PCM). The crossbar and
 * the tuner are added by the capture graph builder.
 *
 * Build: scripts/build-wintv.bat (MSVC) or scripts/build-all.sh (Zig).
 */
#include "tvplat.hpp"

#include <dshow.h>
#include <initguid.h>

#include "tvcore.hpp"

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")

/* ---- DirectShow pieces the SDK no longer ships (qedit.h) ---------------------------------------- */
DEFINE_GUID(CLSID_SampleGrabber, 0xC1F400A0, 0x3F08, 0x11D3, 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(CLSID_NullRenderer, 0xC1F400A4, 0x3F08, 0x11D3, 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(MY_IID_ISampleGrabber, 0x6B652FFF, 0x11FE, 0x4FCE, 0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F);
DEFINE_GUID(MY_IID_ISampleGrabberCB, 0x0579154A, 0x2B53, 0x4994, 0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85);
DEFINE_GUID(MY_KSDATAFORMAT_TYPE_VBI, 0xF72A76E1, 0xEB0A, 0x11D0, 0xAC, 0xE4, 0x00, 0x00, 0xC0, 0xCC, 0x16, 0xBA);
DEFINE_GUID(MY_KSDATAFORMAT_SUBTYPE_RAW8, 0xCA20D9A0, 0x3E3E, 0x11D1, 0x9B, 0xF9, 0x00, 0xC0, 0x4F, 0xBB, 0xDE, 0xBF);

MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample *pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE *pBuffer, long BufferLen) = 0;
};
MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *pBufferSize, long *pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB *pCallback, long WhichMethodToCallback) = 0;
};

struct KS_VBIINFOHEADER_ {
    ULONG StartLine, EndLine, SamplingFrequency, MinLineStartTime, MaxLineStartTime, ActualLineStartTime,
        ActualLineEndTime, VideoStandard, SamplesPerLine, StrideInBytes, BufferSize;
};

template <class T> struct ComPtr {
    T *p = nullptr;
    ComPtr() {}
    ComPtr(T *adopt) : p(adopt) {} /* takes over one reference */
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;
    ComPtr(ComPtr &&o) noexcept : p(o.p) { o.p = nullptr; }
    ComPtr &operator=(ComPtr &&o) noexcept { if (p != o.p) { reset(); p = o.p; o.p = nullptr; } return *this; }
    ~ComPtr() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T **operator&() { reset(); return &p; }
    T *operator->() const { return p; }
    operator T *() const { return p; }
};

static void free_mt(AM_MEDIA_TYPE *mt)
{
    if (!mt) return;
    if (mt->pbFormat) CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk) mt->pUnk->Release();
    CoTaskMemFree(mt);
}

/* ---- device lookup ------------------------------------------------------------------------------ */
struct Dev { std::string name; ComPtr<IMoniker> mon; };

static std::vector<Dev> enum_devices(const GUID &cat)
{
    std::vector<Dev> v;
    ComPtr<ICreateDevEnum> de;
    CoCreateInstance(CLSID_SystemDeviceEnum, 0, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void **)&de);
    if (!de) return v;
    ComPtr<IEnumMoniker> em;
    if (de->CreateClassEnumerator(cat, &em, 0) != S_OK) return v;
    IMoniker *m;
    while (em->Next(1, &m, 0) == S_OK) {
        Dev d;
        d.mon.p = m;
        ComPtr<IPropertyBag> pb;
        if (SUCCEEDED(m->BindToStorage(0, 0, IID_IPropertyBag, (void **)&pb))) {
            VARIANT var;
            VariantInit(&var);
            if (SUCCEEDED(pb->Read(L"FriendlyName", &var, 0))) { d.name = plat::narrow(var.bstrVal); VariantClear(&var); }
        }
        v.push_back(std::move(d));
    }
    return v;
}

static IPin *find_pin(IBaseFilter *f, PIN_DIRECTION dir, const char *name_contains)
{
    ComPtr<IEnumPins> ep;
    if (FAILED(f->EnumPins(&ep))) return nullptr;
    IPin *p;
    while (ep->Next(1, &p, 0) == S_OK) {
        PIN_INFO pi;
        p->QueryPinInfo(&pi);
        if (pi.pFilter) pi.pFilter->Release();
        if (pi.dir == dir && icontains(plat::narrow(pi.achName), name_contains)) return p;
        p->Release();
    }
    return nullptr;
}

/* ---- sample grabber glue ------------------------------------------------------------------------ */
struct GrabCB : ISampleGrabberCB {
    std::function<void(IMediaSample *)> fn;
    STDMETHODIMP QueryInterface(REFIID r, void **pp) override
    {
        if (r == IID_IUnknown || r == MY_IID_ISampleGrabberCB) { *pp = this; return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; } /* lives as long as the backend */
    STDMETHODIMP SampleCB(double, IMediaSample *s) override { if (fn) fn(s); return S_OK; }
    STDMETHODIMP BufferCB(double, BYTE *, long) override { return E_NOTIMPL; }
};

struct Grabber {
    ComPtr<IBaseFilter> filter;
    ComPtr<ISampleGrabber> sg;
    ComPtr<IBaseFilter> null_r;
};

static bool make_grabber(IGraphBuilder *g, Grabber &gr, const GUID &major, const GUID &sub, GrabCB *cb)
{
    if (FAILED(CoCreateInstance(CLSID_SampleGrabber, 0, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void **)&gr.filter))) return false;
    gr.filter->QueryInterface(MY_IID_ISampleGrabber, (void **)&gr.sg);
    if (!gr.sg) return false;
    AM_MEDIA_TYPE mt;
    memset(&mt, 0, sizeof mt);
    mt.majortype = major;
    mt.subtype = sub;
    gr.sg->SetMediaType(&mt);
    gr.sg->SetBufferSamples(FALSE);
    gr.sg->SetCallback(cb, 0);
    if (FAILED(CoCreateInstance(CLSID_NullRenderer, 0, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void **)&gr.null_r))) return false;
    g->AddFilter(gr.filter, L"Grabber");
    g->AddFilter(gr.null_r, L"Null");
    return true;
}

/* stream time of a sample in 100 ns; falls back to arrival time */
static i64 sample_time(IMediaSample *s, i64 arrival_rel)
{
    REFERENCE_TIME a = 0, b = 0;
    HRESULT hr = s->GetTime(&a, &b);
    return (hr == S_OK || hr == VFW_S_NO_STOP_TIME) ? (i64)a : arrival_rel;
}

/* ---- the backend --------------------------------------------------------------------------------- */
class DsBackend : public Backend {
public:
    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> cgb;
    ComPtr<IMediaControl> mc;
    ComPtr<IBaseFilter> vcap, acap;
    ComPtr<IAMTVTuner> tuner;
    Grabber gv, ga, gb;
    GrabCB cbv, cba, cbb;
    KS_VBIINFOHEADER_ vbih{};
    Engine *E = nullptr;
    i64 vbi_prev_t = 0;
    int vbi_started = 0;

    bool open(Engine &e) override;
    bool run(Engine &e) override;
    void stop(Engine &) override { if (mc) mc->Stop(); }
    void close(Engine &) override;
    bool route(Engine &e) override;
    bool tune(Engine &e) override;
    void list_devices() override;
    bool has_tuner(Engine &) override { return tuner.p != nullptr; }

private:
    void on_video(IMediaSample *s);
    void on_audio(IMediaSample *s);
    void on_vbi(IMediaSample *s);
};

void DsBackend::list_devices()
{
    auto v = enum_devices(CLSID_VideoInputDeviceCategory);
    printf("Video capture devices:\n");
    for (auto &d : v) printf("  %s\n", d.name.c_str());
    auto a = enum_devices(CLSID_AudioInputDeviceCategory);
    printf("Audio capture devices:\n");
    for (auto &d : a) printf("  %s\n", d.name.c_str());
}

bool DsBackend::route(Engine &e)
{
    const Options &o = e.o;
    ComPtr<IAMCrossbar> xbar;
    if (FAILED(cgb->FindInterface(&LOOK_UPSTREAM_ONLY, nullptr, vcap, IID_IAMCrossbar, (void **)&xbar)) || !xbar) {
        logf("note: no crossbar found, using the driver's default input");
        return true;
    }
    long nout = 0, nin = 0;
    xbar->get_PinCounts(&nout, &nin);
    long want = (o.source == "tuner") ? PhysConn_Video_Tuner : (o.source == "svideo") ? PhysConn_Video_SVideo : PhysConn_Video_Composite;
    long vin = -1, vout = -1, aout = -1, ain = -1;
    for (long i = 0; i < nin; i++) {
        long rel, type;
        xbar->get_CrossbarPinInfo(TRUE, i, &rel, &type);
        if (type == want) { vin = i; ain = rel; break; }
    }
    for (long i = 0; i < nout; i++) {
        long rel, type;
        xbar->get_CrossbarPinInfo(FALSE, i, &rel, &type);
        if (type == PhysConn_Video_VideoDecoder && vout < 0) vout = i;
        if (type == PhysConn_Audio_AudioDecoder && aout < 0) aout = i;
    }
    if (vin < 0 || vout < 0) { e.error = fmt_str("this device has no '%s' input", o.source.c_str()); return false; }
    xbar->Route(vout, vin);
    if (aout >= 0 && ain >= 0) xbar->Route(aout, ain);
    ComPtr<IAMAnalogVideoDecoder> dec;
    if (SUCCEEDED(cgb->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, vcap, IID_IAMAnalogVideoDecoder, (void **)&dec)) && dec)
        dec->put_TVFormat(o.pal ? AnalogVideo_PAL_B : AnalogVideo_NTSC_M); /* may be refused; the driver then keeps its own */
    return true;
}

bool DsBackend::tune(Engine &e)
{
    const Options &o = e.o;
    if (o.source != "tuner") return true;
    if (!tuner) {
        if (FAILED(cgb->FindInterface(&LOOK_UPSTREAM_ONLY, nullptr, vcap, IID_IAMTVTuner, (void **)&tuner)) || !tuner) {
            e.error = "no TV tuner found";
            return false;
        }
        tuner->put_CountryCode(1);
        tuner->put_TuningSpace(0);
        tuner->put_Mode(AMTUNER_MODE_TV);
    }
    tuner->put_InputType(0, o.cable ? TunerInputCable : TunerInputAntenna);
    if (o.channel <= 0) { e.error = "choose a channel"; return false; }
    HRESULT hr = tuner->put_Channel(o.channel, AMTUNER_SUBCHAN_DEFAULT, AMTUNER_SUBCHAN_DEFAULT);
    if (FAILED(hr)) logf("warning: tuner refused channel %d (0x%08lx)", o.channel, hr);
    return true;
}

bool DsBackend::open(Engine &e)
{
    E = &e;
    const Options &o = e.o;
    cbv.fn = [this](IMediaSample *s) { on_video(s); };
    cba.fn = [this](IMediaSample *s) { on_audio(s); };
    cbb.fn = [this](IMediaSample *s) { on_vbi(s); };
    e.have_vbi = e.have_audio = false;
    vbi_started = 0;
    if (FAILED(CoCreateInstance(CLSID_FilterGraph, 0, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void **)&graph))) { e.error = "no DirectShow"; return false; }
    CoCreateInstance(CLSID_CaptureGraphBuilder2, 0, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void **)&cgb);
    if (!cgb) { e.error = "no DirectShow"; return false; }
    cgb->SetFiltergraph(graph);
    graph->QueryInterface(IID_IMediaControl, (void **)&mc);

    auto vdevs = enum_devices(CLSID_VideoInputDeviceCategory);
    Dev *vd = nullptr;
    for (auto &d : vdevs) if (icontains(d.name, o.device)) { vd = &d; break; }
    if (!vd) { e.error = fmt_str("No capture device with '%s' in its name. Plug in the tuner.", o.device.c_str()); return false; }
    logf("video device: %s", vd->name.c_str());
    if (FAILED(vd->mon->BindToObject(0, 0, IID_IBaseFilter, (void **)&vcap))) { e.error = "Cannot open the tuner. Close WinTV or any other program using it."; return false; }
    graph->AddFilter(vcap, L"Video Capture");

    /* picture format: NTSC 720x480 or PAL 720x576 */
    ComPtr<IPin> vpin;
    cgb->FindPin(vcap, PINDIR_OUTPUT, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, FALSE, 0, &vpin);
    if (vpin) {
        ComPtr<IAMStreamConfig> sc;
        vpin->QueryInterface(IID_IAMStreamConfig, (void **)&sc);
        int n = 0, sz = 0;
        if (sc && SUCCEEDED(sc->GetNumberOfCapabilities(&n, &sz))) {
            std::vector<BYTE> caps(sz);
            for (int i = 0; i < n; i++) {
                AM_MEDIA_TYPE *mt = nullptr;
                if (FAILED(sc->GetStreamCaps(i, &mt, caps.data()))) continue;
                if (mt->formattype == FORMAT_VideoInfo && mt->subtype == MEDIASUBTYPE_YUY2) {
                    VIDEOINFOHEADER *vi = (VIDEOINFOHEADER *)mt->pbFormat;
                    if (abs(vi->bmiHeader.biHeight) == (o.pal ? 576 : 480)) { sc->SetFormat(mt); free_mt(mt); break; }
                }
                free_mt(mt);
            }
        }
    }
    e.pixfmt = "yuyv422";
    if (o.pal) { e.fps_num = 25; e.fps_den = 1; e.frame_dur = 400000; } else { e.fps_num = 30000; e.fps_den = 1001; e.frame_dur = 333667; }

    /* video chain: capture -> grabber -> null (this also adds the crossbar and tuner) */
    if (!make_grabber(graph, gv, MEDIATYPE_Video, MEDIASUBTYPE_YUY2, &cbv)) { e.error = "cannot create the video grabber"; return false; }
    HRESULT hr = cgb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, vcap, gv.filter, gv.null_r);
    if (FAILED(hr)) { e.error = fmt_str("Cannot connect the video (0x%08lx). Close any other program using the tuner.", hr); return false; }
    if (!route(e)) return false;
    if (!tune(e)) return false;
    {
        AM_MEDIA_TYPE mt;
        if (SUCCEEDED(gv.sg->GetConnectedMediaType(&mt)) && mt.formattype == FORMAT_VideoInfo) {
            VIDEOINFOHEADER *vi = (VIDEOINFOHEADER *)mt.pbFormat;
            e.vw = vi->bmiHeader.biWidth;
            e.vh = abs(vi->bmiHeader.biHeight);
            e.vbytes = vi->bmiHeader.biSizeImage ? vi->bmiHeader.biSizeImage : (size_t)e.vw * e.vh * 2;
            logf("video: %dx%d, %zu bytes/frame", e.vw, e.vh, e.vbytes);
            CoTaskMemFree(mt.pbFormat);
        }
    }

    /* raw VBI pin */
    if (!o.no_cc || o.probe > 0) {
        ComPtr<IPin> pv = find_pin(vcap, PINDIR_OUTPUT, "VBI");
        if (pv) {
            if (make_grabber(graph, gb, MY_KSDATAFORMAT_TYPE_VBI, MY_KSDATAFORMAT_SUBTYPE_RAW8, &cbb)) {
                ComPtr<IPin> gin = find_pin(gb.filter, PINDIR_INPUT, "");
                ComPtr<IPin> gout = find_pin(gb.filter, PINDIR_OUTPUT, "");
                ComPtr<IPin> nin = find_pin(gb.null_r, PINDIR_INPUT, "");
                HRESULT h1 = graph->ConnectDirect(pv, gin, nullptr);
                if (SUCCEEDED(h1) && gout && nin) graph->ConnectDirect(gout, nin, nullptr);
                if (SUCCEEDED(h1)) {
                    AM_MEDIA_TYPE mt;
                    if (SUCCEEDED(gb.sg->GetConnectedMediaType(&mt)) && mt.cbFormat >= sizeof(KS_VBIINFOHEADER_)) {
                        memcpy(&vbih, mt.pbFormat, sizeof vbih);
                        e.have_vbi = true;
                        e.vbi_fs = vbih.SamplingFrequency ? vbih.SamplingFrequency : 27e6;
                        logf("vbi: lines %lu-%lu, %lu samples/line at %.3f MHz, buffer %lu bytes (stride %lu)", vbih.StartLine,
                             vbih.EndLine, vbih.SamplesPerLine, e.vbi_fs / 1e6, vbih.BufferSize, vbih.StrideInBytes);
                    }
                    if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
                } else logf("warning: could not connect the VBI pin (0x%08lx); no captions", h1);
            }
        } else logf("warning: this driver has no VBI pin; no captions");
    }

    /* audio */
    auto adevs = enum_devices(CLSID_AudioInputDeviceCategory);
    Dev *ad = nullptr;
    for (auto &d : adevs) if (icontains(d.name, o.audio_device)) { ad = &d; break; }
    if (ad) {
        logf("audio device: %s", ad->name.c_str());
        if (SUCCEEDED(ad->mon->BindToObject(0, 0, IID_IBaseFilter, (void **)&acap))) {
            graph->AddFilter(acap, L"Audio Capture");
            {   /* the USB audio device's input mixer is disabled by default and records silence until it is on */
                ComPtr<IEnumPins> ep;
                if (SUCCEEDED(acap->EnumPins(&ep))) {
                    IPin *ip;
                    while (ep->Next(1, &ip, 0) == S_OK) {
                        PIN_INFO pi;
                        ip->QueryPinInfo(&pi);
                        if (pi.pFilter) pi.pFilter->Release();
                        if (pi.dir == PINDIR_INPUT) {
                            ComPtr<IAMAudioInputMixer> mx;
                            ip->QueryInterface(IID_IAMAudioInputMixer, (void **)&mx);
                            if (mx) {
                                BOOL en = FALSE; double lvl = -1;
                                mx->get_Enable(&en);
                                mx->get_MixLevel(&lvl);
                                logf("audio mixer pin \"%s\": enabled=%d level=%.2f", plat::narrow(pi.achName).c_str(), (int)en, lvl);
                                mx->put_Enable(TRUE);
                                mx->put_MixLevel(1.0);
                            }
                        }
                        ip->Release();
                    }
                }
            }
            ComPtr<IPin> apin;
            cgb->FindPin(acap, PINDIR_OUTPUT, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, FALSE, 0, &apin);
            if (apin) {
                {   /* 20 ms buffers instead of the driver's half-second ones: smooth monitoring, less delay */
                    ComPtr<IAMBufferNegotiation> bn;
                    apin->QueryInterface(IID_IAMBufferNegotiation, (void **)&bn);
                    if (bn) {
                        ALLOCATOR_PROPERTIES ap;
                        ap.cBuffers = 12;
                        ap.cbBuffer = 48000 * 4 / 50;
                        ap.cbAlign = -1;
                        ap.cbPrefix = -1;
                        HRESULT hb = bn->SuggestAllocatorProperties(&ap);
                        logf("audio buffers: asked for %d x %d bytes (0x%08lx)", (int)ap.cBuffers, (int)ap.cbBuffer, hb);
                    }
                }
                ComPtr<IAMStreamConfig> sc;
                apin->QueryInterface(IID_IAMStreamConfig, (void **)&sc);
                int n = 0, sz = 0;
                if (sc && SUCCEEDED(sc->GetNumberOfCapabilities(&n, &sz))) {
                    std::vector<BYTE> caps(sz);
                    for (int i = 0; i < n; i++) {
                        AM_MEDIA_TYPE *mt = nullptr;
                        if (FAILED(sc->GetStreamCaps(i, &mt, caps.data()))) continue;
                        if (mt->formattype == FORMAT_WaveFormatEx) {
                            WAVEFORMATEX *w = (WAVEFORMATEX *)mt->pbFormat;
                            if (w->wFormatTag == WAVE_FORMAT_PCM && w->nChannels == 2 && w->wBitsPerSample == 16 && w->nSamplesPerSec == 48000) {
                                sc->SetFormat(mt);
                                free_mt(mt);
                                break;
                            }
                        }
                        free_mt(mt);
                    }
                }
            }
            if (make_grabber(graph, ga, MEDIATYPE_Audio, MEDIASUBTYPE_PCM, &cba) &&
                SUCCEEDED(cgb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, acap, ga.filter, ga.null_r))) {
                AM_MEDIA_TYPE mt;
                if (SUCCEEDED(ga.sg->GetConnectedMediaType(&mt)) && mt.formattype == FORMAT_WaveFormatEx) {
                    WAVEFORMATEX *w = (WAVEFORMATEX *)mt.pbFormat;
                    e.audio.rate = (int)w->nSamplesPerSec; e.audio.channels = w->nChannels; e.audio.bits = w->wBitsPerSample;
                    e.have_audio = true;
                    logf("audio: %d Hz, %d ch, %d bit", e.audio.rate, e.audio.channels, e.audio.bits);
                }
                if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
            } else logf("warning: could not connect the audio device");
        }
    } else logf("warning: no audio device with '%s' in its name; recording without sound", o.audio_device.c_str());
    return true;
}

bool DsBackend::run(Engine &e)
{
    HRESULT hr = mc->Run();
    if (FAILED(hr)) {
        e.error = hr == HRESULT_FROM_WIN32(ERROR_GEN_FAILURE)
                      ? "The tuner is not responding. Unplug and replug its USB cable."
                      : fmt_str("Cannot start the tuner (0x%08lx).", hr);
        return false;
    }
    if (hr == S_FALSE) { OAFilterState st; mc->GetState(3000, &st); }
    return true;
}

void DsBackend::close(Engine &)
{
    if (mc) mc->Stop();
    tuner.reset();
    gv = Grabber(); ga = Grabber(); gb = Grabber();
    acap.reset(); vcap.reset(); mc.reset(); cgb.reset(); graph.reset();
}

void DsBackend::on_video(IMediaSample *s)
{
    BYTE *p = nullptr;
    s->GetPointer(&p);
    E->video(sample_time(s, E->rel_now()), p, s->GetActualDataLength());
}

void DsBackend::on_audio(IMediaSample *s)
{
    BYTE *p = nullptr;
    s->GetPointer(&p);
    E->audio_in(sample_time(s, E->rel_now()), p, s->GetActualDataLength());
}

/* The driver delivers one VBI buffer per field, in display order; the two buffers of a frame carry nearly the
 * same time stamp (about 0.1 ms apart), the next frame follows ~33 ms later. So the first buffer after a gap is
 * the first field of its frame: field 2 for bottom-field-first (the default), field 1 for top-field-first.
 * Counting buffers instead would swap the fields after any lost buffer. */
void DsBackend::on_vbi(IMediaSample *s)
{
    E->cb_b++;
    BYTE *p = nullptr;
    s->GetPointer(&p);
    long n = s->GetActualDataLength();
    if (!p || n <= 0) return;
    i64 t = sample_time(s, E->rel_now());
    int stride = vbih.StrideInBytes ? (int)vbih.StrideInBytes : (int)vbih.SamplesPerLine;
    int lines = stride ? n / stride : 0;
    bool first_of_frame = vbi_started == 0 || (t - vbi_prev_t) > E->frame_dur / 2;
    vbi_started = 1;
    vbi_prev_t = t;
    int field = first_of_frame ? (E->o.bff ? 2 : 1) : (E->o.bff ? 1 : 2);
    E->vbi_push(field, t, p, lines, stride, (int)vbih.SamplesPerLine, E->vbi_fs);
}

int main(int argc, char **argv)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);
    DsBackend be;
    return tv_main(argc, argv, be);
}
