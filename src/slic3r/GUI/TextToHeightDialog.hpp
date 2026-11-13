#ifndef slic3r_GUI_TextToHeightDialog_hpp_
#define slic3r_GUI_TextToHeightDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/TextToHeight.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class wxTextCtrl;
class wxStaticText;
class wxCheckBox;
class wxGridSizer;
class Button;
class SpinInput;
class ProgressBar;

namespace Slic3r { namespace GUI {

// Describe a texture in words, get a few seamless height maps to choose from (made on this computer, see
// libslic3r/TextToHeight.hpp), and keep one as the displacement map image.
class TextToHeightDialog : public DPIDialog
{
public:
    explicit TextToHeightDialog(wxWindow* parent);
    ~TextToHeightDialog() override;

    // The 16-bit PNG written for the chosen texture (forward slashes); empty until OK.
    const std::string& height_map_path() const { return m_path; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    struct Variant
    {
        int64_t                seed = 0;
        TextToHeight::ImageRGB colour;
        std::vector<float>     height; // 0..1, colour.width x colour.height
    };

    void start();
    void stop_worker();
    void select(int i);
    void update_tiles();
    void fit();
    void set_busy(bool busy);
    bool save_selected();

    TextToHeight::ModelFiles m_files;
    std::string              m_path;

    wxTextCtrl*                  m_prompt   = nullptr;
    SpinInput*                   m_variants = nullptr;
    SpinInput*                   m_seed     = nullptr;
    Button*                      m_generate = nullptr;
    Button*                      m_ok       = nullptr;
    wxCheckBox*                  m_colour   = nullptr;
    wxStaticText*                m_status   = nullptr;
    wxStaticText*                m_credit   = nullptr;
    ProgressBar*                 m_progress = nullptr;
    wxGridSizer*                 m_grid     = nullptr;
    std::vector<wxWindow*>       m_tiles;

    std::vector<Variant> m_results;
    std::string          m_prompt_used;
    int                  m_selected = -1;

    // The models stay loaded while the dialog is open, so further prompts are quick.
    std::unique_ptr<TextToHeight::ColourGenerator> m_colour_gen;
    std::unique_ptr<TextToHeight::NormalEstimator> m_normals;
    std::thread                                    m_worker;
    std::atomic<bool>                              m_cancel{false};
    bool                                           m_busy = false;
};

}} // namespace Slic3r::GUI

#endif
