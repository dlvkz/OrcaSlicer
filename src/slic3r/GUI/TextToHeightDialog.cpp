#include "TextToHeightDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/ProgressBar.hpp"
#include "Widgets/SpinInput.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>

#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/rawbmp.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cstdlib>
#include <random>

namespace Slic3r { namespace GUI {

namespace {
const int TILE_DIP = 180;

// The credits the SD-Turbo licence asks for, and where it runs: "processor" or the graphics card's name.
// The graphics card compiles its programs on the first texture of each run of the slicer: a few seconds.
std::atomic<bool> s_gpu_ready{false};

wxString credit_text(const wxString& device)
{
    const wxString where = device == "processor" ? _L("on the processor") : wxString::Format(_L("on %s"), device);
    return wxString::Format(_L("Powered by Stability AI (SD-Turbo, 4-bit). Normal maps by DeepBump. Runs locally %s."), where);
}

// The texture shrunk to px x px (box filter), written straight into the bitmap's pixels: going through wxImage gave
// black bitmaps here. Set ORCA_T2H_DUMP to a folder to save each thumbnail there as PNG, from the pixels and back from
// the bitmap, for checking.
wxBitmap thumbnail(const TextToHeight::ImageRGB& colour, const std::vector<float>& height, bool show_colour, int px)
{
    const int W = colour.width, H = colour.height;
    if (W <= 0 || H <= 0 || px <= 0 || height.size() < size_t(W) * H || (show_colour && colour.rgb.size() < size_t(W) * H * 3))
        return wxNullBitmap;
    std::vector<unsigned char> rgb(size_t(px) * px * 3);
    for (int y = 0; y < px; ++y) {
        const int y0 = y * H / px, y1 = std::max(y0 + 1, (y + 1) * H / px);
        for (int x = 0; x < px; ++x) {
            const int x0 = x * W / px, x1 = std::max(x0 + 1, (x + 1) * W / px);
            float     sum[3] = {0.f, 0.f, 0.f};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const size_t i = size_t(sy) * W + sx;
                    for (int c = 0; c < 3; ++c)
                        sum[c] += show_colour ? float(colour.rgb[i * 3 + c]) : std::clamp(height[i], 0.f, 1.f) * 255.f;
                }
            const float n = float((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 3; ++c)
                rgb[(size_t(y) * px + x) * 3 + c] = (unsigned char) std::clamp(std::lround(sum[c] / n), 0L, 255L);
        }
    }
    wxBitmap bmp(px, px, 24);
    {
        wxNativePixelData data(bmp);
        if (!data)
            return wxNullBitmap;
        wxNativePixelData::Iterator p(data);
        for (int y = 0; y < px; ++y) {
            wxNativePixelData::Iterator row = p;
            for (int x = 0; x < px; ++x, ++p) {
                const unsigned char* s = &rgb[(size_t(y) * px + x) * 3];
                p.Red()                = s[0];
                p.Green()              = s[1];
                p.Blue()               = s[2];
            }
            p = row;
            p.OffsetY(data, 1);
        }
    }
    if (const char* dump = std::getenv("ORCA_T2H_DUMP")) {
        static int n = 0;
        const std::string base = std::string(dump) + "/thumb_" + std::to_string(n++);
        wxImage           img(px, px, false);
        std::copy(rgb.begin(), rgb.end(), img.GetData());
        img.SaveFile(from_u8(base + "_pixels.png"), wxBITMAP_TYPE_PNG);
        bmp.ConvertToImage().SaveFile(from_u8(base + "_bitmap.png"), wxBITMAP_TYPE_PNG);
    }
    return bmp;
}

// One texture to choose: the picture, its seed below it, and an accent frame when it is the chosen one. Painted by hand
// (a static bitmap in a coloured panel came out black and did not follow the dark theme).
class TextureTile : public wxWindow
{
public:
    TextureTile(wxWindow* parent, int px) : wxWindow(parent, wxID_ANY), m_px(px)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(Label::Body_12);
        const int pad = parent->FromDIP(4);
        SetMinSize(wxSize(px + 2 * pad, px + 2 * pad + GetCharHeight() + pad));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &TextureTile::on_paint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(); });
    }
    void set(const wxBitmap& bmp, const wxString& label, bool selected)
    {
        m_bmp      = bmp;
        m_label    = label;
        m_selected = selected;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const bool  dark   = wxGetApp().dark_mode();
        const wxColour bg  = GetParent()->GetBackgroundColour();
        const wxColour accent(0, 150, 136);
        const int   pad    = FromDIP(4);
        const wxSize sz    = GetClientSize();
        dc.SetBackground(wxBrush(bg));
        dc.Clear();
        if (m_selected || (m_hover && m_bmp.IsOk())) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(m_selected ? accent : (dark ? wxColour(70, 70, 76) : wxColour(220, 220, 220))));
            dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, FromDIP(4));
        }
        if (m_bmp.IsOk())
            dc.DrawBitmap(m_bmp, pad, pad, false);
        else {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(dark ? wxColour(58, 58, 63) : wxColour(238, 238, 238)));
            dc.DrawRectangle(pad, pad, m_px, m_px);
        }
        if (!m_label.empty()) {
            dc.SetFont(GetFont());
            dc.SetTextForeground(m_selected ? *wxWHITE : (dark ? wxColour(220, 220, 220) : wxColour(60, 60, 60)));
            dc.DrawText(m_label, pad, pad + m_px + pad / 2);
        }
    }

    int      m_px;
    wxBitmap m_bmp;
    wxString m_label;
    bool     m_selected = false;
    bool     m_hover    = false;
};
} // namespace

TextToHeightDialog::TextToHeightDialog(wxWindow* parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Displacement map from text"),
                wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_files(TextToHeight::ModelFiles::in_folder(TextToHeight::default_model_folder()))
    , m_colour_gen(new TextToHeight::ColourGenerator)
    , m_normals(new TextToHeight::NormalEstimator)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    const int gap = FromDIP(10);

    auto v = new wxBoxSizer(wxVERTICAL);

    auto intro = new wxStaticText(this, wxID_ANY,
                                  _L("Describe a surface, for example \"cobblestone pavement\" or \"dragon scales\". "
                                     "A few seamless height maps are made from it locally; pick one to use as the displacement map."));
    intro->Wrap(FromDIP(4 * TILE_DIP + 30));
    v->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    auto row = new wxBoxSizer(wxHORIZONTAL);
    m_prompt = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(360), -1), wxTE_PROCESS_ENTER);
    m_prompt->SetHint(_L("cobblestone pavement"));
    m_prompt->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { start(); });
    row->Add(m_prompt, 1, wxALIGN_CENTER_VERTICAL);
    m_generate = new Button(this, _L("Generate"));
    m_generate->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_generate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_busy)
            stop_worker();
        else
            start();
    });
    row->Add(m_generate, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, gap);
    v->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    auto opts = new wxBoxSizer(wxHORIZONTAL);
    opts->Add(new wxStaticText(this, wxID_ANY, _L("Variants") + ":"), 0, wxALIGN_CENTER_VERTICAL);
    m_variants = new SpinInput(this, wxEmptyString, "", wxDefaultPosition, wxSize(FromDIP(70), -1), wxSP_ARROW_KEYS, 1, 8, 4);
    opts->Add(m_variants, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    opts->Add(new wxStaticText(this, wxID_ANY, _L("Seed") + ":"), 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 2 * gap);
    m_seed = new SpinInput(this, wxEmptyString, "", wxDefaultPosition, wxSize(FromDIP(110), -1), wxSP_ARROW_KEYS, 0, 999999,
                           int(std::random_device{}() % 100000));
    m_seed->SetToolTip(_L("The same description and seed give the same texture on the same computer. Variants use the following seeds."));
    opts->Add(m_seed, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(5));
    m_colour = new wxCheckBox(this, wxID_ANY, _L("Show colour"));
    m_colour->SetToolTip(_L("The colour image the height map was made from. Only the height map is used."));
    m_colour->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { update_tiles(); });
    opts->AddStretchSpacer();
    opts->Add(m_colour, 0, wxALIGN_CENTER_VERTICAL);
    v->Add(opts, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    m_grid = new wxGridSizer(0, 4, FromDIP(6), FromDIP(6));
    v->Add(m_grid, 0, wxALL, gap);

    m_status = new wxStaticText(this, wxID_ANY, "");
    v->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);
    m_progress = new ProgressBar(this, wxID_ANY, 100);
    m_progress->SetHeight(FromDIP(8));
    m_progress->SetMaxSize(wxSize(-1, FromDIP(8)));
    m_progress->SetDoubleBuffered(true);
    m_progress->Hide();
    v->Add(m_progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    m_credit = new wxStaticText(this, wxID_ANY, credit_text(from_u8(TextToHeight::compute_device_description())));
    m_credit->SetFont(Label::Body_12);
    m_credit->SetForegroundColour(wxColour(120, 120, 120));
    v->Add(m_credit, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    auto bottom = new wxBoxSizer(wxHORIZONTAL);
    auto folder = new Button(this, _L("Model folder"));
    folder->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    folder->SetToolTip(from_u8(TextToHeight::default_model_folder()));
    folder->Bind(wxEVT_BUTTON, [](wxCommandEvent&) {
        boost::system::error_code ec;
        boost::filesystem::create_directories(TextToHeight::default_model_folder(), ec);
        desktop_open_any_folder(TextToHeight::default_model_folder());
    });
    bottom->Add(folder, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, gap);
    auto btns = new DialogButtons(this, {"OK", "Cancel"});
    m_ok      = btns->GetOK();
    m_ok->SetLabel(_L("Use this texture"));
    m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (save_selected())
            EndModal(wxID_OK);
    });
    btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        stop_worker();
        EndModal(wxID_CANCEL);
    });
    bottom->Add(btns, 1, wxEXPAND);
    v->Add(bottom, 0, wxEXPAND | wxTOP, gap);

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        stop_worker();
        e.Skip();
    });

    SetSizer(v);
    m_ok->Enable(false);

    const std::vector<std::string> missing = m_files.missing();
    if (!missing.empty()) {
        std::string names;
        for (const std::string& n : missing)
            names += (names.empty() ? "" : ", ") + n;
        m_status->SetLabel(wxString::Format(_L("The model files are not installed (%s). Put them in the model folder: %s"),
                                            from_u8(names), from_u8(TextToHeight::default_model_folder())));
        m_status->Wrap(FromDIP(4 * TILE_DIP + 30));
        m_generate->Enable(false);
    }
    update_tiles();
    Layout();
    v->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
    // Texts and the check box take the dialog's background; left alone they get a box of their own in the dark theme.
    for (wxWindow* child : GetChildren())
        if (dynamic_cast<wxStaticText*>(child) || dynamic_cast<wxCheckBox*>(child))
            child->SetBackgroundColour(GetBackgroundColour());
    m_prompt->SetFocus();
}

TextToHeightDialog::~TextToHeightDialog() { stop_worker(); }

void TextToHeightDialog::on_dpi_changed(const wxRect&)
{
    update_tiles();
    Refresh();
}

void TextToHeightDialog::set_busy(bool busy)
{
    m_busy = busy;
    m_generate->SetLabel(busy ? _L("Stop") : _L("Generate"));
    m_prompt->Enable(!busy);
    m_variants->Enable(!busy);
    m_seed->Enable(!busy);
    m_progress->Show(busy);
    if (!busy)
        m_progress->SetValue(0);
    m_ok->Enable(!busy && m_selected >= 0);
    fit();
}

void TextToHeightDialog::stop_worker()
{
    m_cancel = true;
    m_colour_gen->cancel();
    if (m_worker.joinable())
        m_worker.join();
    if (m_busy)
        set_busy(false);
}

void TextToHeightDialog::start()
{
    if (m_busy || !m_generate->IsEnabled())
        return;
    std::string prompt = into_u8(m_prompt->GetValue());
    boost::algorithm::trim(prompt);
    if (prompt.empty()) {
        m_prompt->SetFocus();
        return;
    }
    if (m_worker.joinable())
        m_worker.join();
    const int     n     = m_variants->GetValue();
    const int64_t seed0 = m_seed->GetValue();
    m_results.clear();
    m_selected    = -1;
    m_prompt_used = prompt;
    update_tiles();
    m_cancel = false;
    set_busy(true);
    m_status->SetLabel(m_colour_gen->loaded() ? _L("Starting...") : _L("Loading the model (about 1 GB of memory)..."));

    m_worker = std::thread([this, prompt, n, seed0]() {
        auto fail = [this](const std::string& error) {
            CallAfter([this, error]() {
                set_busy(false);
                m_status->SetLabel(m_cancel ? _L("Stopped.") : _L("Error") + ": " + from_u8(error));
            });
        };
        std::string error;
        if (!m_colour_gen->loaded() && !m_colour_gen->load(m_files, 0, error))
            return fail(error);
        if (!m_normals->loaded() && !m_normals->load(m_files.deepbump, 0, error))
            return fail(error);
        CallAfter([this, device = m_colour_gen->device()]() { m_credit->SetLabel(credit_text(from_u8(device))); });
        for (int k = 0; k < n && !m_cancel; ++k) {
            const bool preparing = !s_gpu_ready && m_colour_gen->device() != "processor";
            CallAfter([this, k, n, preparing]() {
                m_status->SetLabel(preparing ? wxString::Format(_L("Making texture %d of %d (preparing the graphics card: the first one takes a few seconds longer)..."), k + 1, n) :
                                               wxString::Format(_L("Making texture %d of %d..."), k + 1, n));
                m_progress->SetValue(100 * k / n);
            });
            auto v  = std::make_shared<Variant>();
            v->seed = seed0 + k;
            if (!m_colour_gen->generate(prompt, v->seed, v->colour, error))
                return fail(error);
            s_gpu_ready = true;
            // It may have moved to the processor after failing on the graphics card.
            CallAfter([this, device = m_colour_gen->device()]() { m_credit->SetLabel(credit_text(from_u8(device))); });
            if (m_cancel)
                break;
            CallAfter([this, k, n]() { m_progress->SetValue(int(100 * (k + 0.7) / n)); });
            std::vector<float> normals;
            if (!m_normals->colour_to_normals(v->colour, normals, error, [this](float) { return !m_cancel.load(); }))
                return fail(error);
            v->height = TextToHeight::normals_to_height(normals, v->colour.width, v->colour.height);
            TextToHeight::normalise(v->height);
            CallAfter([this, v]() {
                m_results.push_back(std::move(*v));
                if (m_selected < 0)
                    m_selected = 0;
                update_tiles();
            });
        }
        CallAfter([this]() {
            set_busy(false);
            m_status->SetLabel(m_cancel ? _L("Stopped.") : _L("Click a texture to choose it."));
        });
    });
}

void TextToHeightDialog::select(int i)
{
    if (i < 0 || i >= int(m_results.size()))
        return;
    m_selected = i;
    update_tiles();
    m_ok->Enable(!m_busy);
}

void TextToHeightDialog::update_tiles()
{
    const int px    = FromDIP(TILE_DIP);
    const int count = std::max(4, int(m_results.size()));
    while (int(m_tiles.size()) < count) {
        auto      tile = new TextureTile(this, px);
        const int idx  = int(m_tiles.size());
        tile->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) { select(idx); });
        tile->Bind(wxEVT_LEFT_DCLICK, [this, idx](wxMouseEvent&) {
            select(idx);
            if (!m_busy && save_selected())
                EndModal(wxID_OK);
        });
        m_grid->Add(tile);
        m_tiles.push_back(tile);
    }
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        auto tile = static_cast<TextureTile*>(m_tiles[i]);
        if (i < m_results.size()) {
            const Variant& r = m_results[i];
            tile->set(thumbnail(r.colour, r.height, m_colour->GetValue(), px), wxString::Format(_L("Seed %lld"), (long long) r.seed),
                      int(i) == m_selected);
        } else
            tile->set(wxNullBitmap, "", false);
        tile->Show(i < 4 || i < m_results.size());
    }
    fit();
}

void TextToHeightDialog::fit()
{
    Layout();
    if (GetSizer())
        GetSizer()->Fit(this);
}

bool TextToHeightDialog::save_selected()
{
    if (m_selected < 0 || m_selected >= int(m_results.size()))
        return false;
    const Variant&                r   = m_results[m_selected];
    const boost::filesystem::path dir = boost::filesystem::path(data_dir()) / "textures" / "text_to_height";
    boost::system::error_code     ec;
    boost::filesystem::create_directories(dir, ec);
    std::string path = (dir / (TextToHeight::slug(m_prompt_used) + "_" + std::to_string(r.seed) + "_height16.png")).string();
    std::string error;
    if (!TextToHeight::save_height_png16(path, r.height, r.colour.width, r.colour.height, error)) {
        m_status->SetLabel(_L("Error") + ": " + from_u8(error));
        return false;
    }
    // Forward slashes: string options un-escape backslashes when a project is loaded again.
    std::replace(path.begin(), path.end(), '\\', '/');
    m_path = path;
    return true;
}

}} // namespace Slic3r::GUI
