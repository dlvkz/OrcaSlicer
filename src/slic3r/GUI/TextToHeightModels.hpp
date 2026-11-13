#ifndef slic3r_GUI_TextToHeightModels_hpp_
#define slic3r_GUI_TextToHeightModels_hpp_

class wxWindow;

namespace Slic3r { namespace GUI {

// The text to height model files are an add-on: not part of the program, downloaded from the fork's GitHub release into
// TextToHeight::default_model_folder() when the user first asks for a map from text.
// True when the files are there, already or after the user agreed to download them; false when they declined, stopped
// the download or it failed (they were told why). A stopped or failed download continues where it left off next time.
bool ensure_text_to_height_models(wxWindow* parent);

}} // namespace Slic3r::GUI

#endif
