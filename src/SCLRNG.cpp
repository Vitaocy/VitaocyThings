#include "plugin.hpp"

#include <osdialog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <vector>


static std::string trim(const std::string& s) {
	size_t begin = s.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(begin, end - begin + 1);
}

static bool isSclFile(const std::string& path) {
	size_t dot = path.find_last_of('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = path.substr(dot + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	return ext == "scl";
}

static std::string filename(const std::string& path) {
	size_t slash = path.find_last_of("/\\");
	return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

/** Parses one .scl interval line into cents. */
static bool parseInterval(const std::string& line, float& cents) {
	size_t slash = line.find('/');
	if (slash != std::string::npos) {
		double num = std::atof(line.substr(0, slash).c_str());
		double den = std::atof(line.substr(slash + 1).c_str());
		if (num <= 0 || den <= 0)
			return false;
		cents = 1200.f * std::log2(num / den);
		return true;
	}
	// A negative number is a frequency ratio in the .scl format
	char* end = NULL;
	double value = std::strtod(line.c_str(), &end);
	if (end == line.c_str())
		return false;
	if (value < 0) {
		cents = 1200.f * std::log2(-value);
	}
	else {
		cents = value;
	}
	return true;
}

/** Parses a .scl file into its description and folded interval cents. */
static bool parseScl(const std::string& text, std::string& description, std::vector<float>& cents) {
	std::vector<std::string> lines;
	{
		std::stringstream ss(text);
		std::string line;
		while (std::getline(ss, line))
			lines.push_back(line);
	}

	// First non-comment line is the description, the second is the note count
	int count = -1;
	size_t i = 0;
	for (; i < lines.size(); i++) {
		std::string t = trim(lines[i]);
		if (t.empty() || t[0] == '!')
			continue;
		description = t;
		break;
	}
	for (i++; i < lines.size(); i++) {
		std::string t = trim(lines[i]);
		if (t.empty() || t[0] == '!')
			continue;
		count = std::atoi(t.c_str());
		break;
	}
	if (count < 0)
		return false;

	for (i++; i < lines.size() && (int) cents.size() < count; i++) {
		std::string t = trim(lines[i]);
		if (t.empty() || t[0] == '!')
			continue;
		float c;
		if (parseInterval(t, c))
			cents.push_back(c);
	}
	if (cents.empty())
		return false;

	// Fold everything into one octave, then sort and deduplicate
	for (float& c : cents) {
		c = std::fmod(std::fmod(c, 1200.f) + 1200.f, 1200.f);
	}
	cents.push_back(0.f);
	std::sort(cents.begin(), cents.end());
	std::vector<float> folded;
	for (float c : cents) {
		if (folded.empty() || c - folded.back() > 0.01f)
			folded.push_back(c);
	}
	cents = folded;
	return true;
}


struct SCLRNG : Module {
	enum ParamIds {
		NUM_PARAMS
	};
	enum InputIds {
		PITCH_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		PITCH_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	static constexpr int MAX_HISTORY = 256;
	static constexpr int MAX_TONES = 128;

	std::string sclDir;
	std::vector<std::string> history; // filenames of previously loaded scales
	int index = -1;

	// Current scale state, guarded by scaleMutex
	std::vector<float> scale = {0.f};
	std::vector<int> enabled = {1};
	std::vector<int> playing = {0};
	std::mutex scaleMutex;
	std::string description;
	std::string currentFile;

	SCLRNG() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configInput(PITCH_INPUT, "1V/octave pitch");
		configOutput(PITCH_OUTPUT, "Pitch");
		configBypass(PITCH_INPUT, PITCH_OUTPUT);

		sclDir = asset::plugin(pluginInstance, "scl");
		onReset();
	}

	void onReset() override {
		history.clear();
		index = -1;
		randomScale();
	}

	/** Picks a random .scl file, loads it and appends it to the history. */
	void randomScale() {
		// Cache the file list after the first call, so 5000+ files are not rescanned
		static std::vector<std::string> files;
		static bool filesLoaded = false;
		if (!filesLoaded) {
			for (const std::string& entry : system::getEntries(sclDir, 0)) {
				if (system::isFile(entry) && isSclFile(entry))
					files.push_back(filename(entry));
			}
			filesLoaded = true;
		}

		if (files.empty()) {
			std::lock_guard<std::mutex> lock(scaleMutex);
			scale = {0.f};
			enabled = {1};
			playing = {0};
			description = "No .scl files found";
			currentFile = "";
			return;
		}

		// Try a few random files, avoiding the one currently loaded
		for (int attempt = 0; attempt < 20; attempt++) {
			std::string file = files[random::u32() % files.size()];
			if (file == currentFile && files.size() > 1)
				continue;
			if (loadScale(file)) {
				history.push_back(file);
				index = (int) history.size() - 1;
				if (history.size() > MAX_HISTORY) {
					history.erase(history.begin());
					index--;
				}
				return;
			}
		}

		// Nothing could be parsed, fall back to a plain octave scale
		std::lock_guard<std::mutex> lock(scaleMutex);
		scale = {0.f};
		enabled = {1};
		playing = {0};
		description = "Could not parse any .scl file";
		currentFile = "";
	}

	/** Loads a scale from the scl directory by filename. */
	bool loadScale(const std::string& file) {
		std::string path = system::join(sclDir, file);
		std::vector<uint8_t> data = system::readFile(path);
		if (data.empty())
			return false;
		std::string text(data.begin(), data.end());

		std::string desc;
		std::vector<float> cents;
		if (!parseScl(text, desc, cents))
			return false;
		if ((int) cents.size() > MAX_TONES)
			cents.resize(MAX_TONES);

		std::lock_guard<std::mutex> lock(scaleMutex);
		scale = cents;
		enabled.assign(cents.size(), 1);
		playing.assign(cents.size(), 0);
		description = desc;
		currentFile = file;
		return true;
	}

	/** Inserts a scale right after the current one and selects it. */
	void chooseScale(const std::string& chosenPath) {
		// Only .scl files are accepted
		if (!isSclFile(chosenPath))
			return;
		std::string file = filename(chosenPath);

		// Only files inside the scl folder are kept; copy others into it
		std::string inDir = system::join(sclDir, file);
		if (!system::isFile(inDir)) {
			if (system::isFile(chosenPath))
				system::copy(chosenPath, inDir);
		}
		if (!system::isFile(inDir))
			return;

		history.insert(history.begin() + index + 1, file);
		index++;
		if (history.size() > MAX_HISTORY) {
			history.erase(history.begin());
			index--;
		}
		loadScale(file);
	}

	void prevScale() {
		if (index > 0) {
			index--;
			loadScale(history[index]);
		}
	}

	void nextScale() {
		if (index + 1 < (int) history.size()) {
			index++;
			loadScale(history[index]);
		}
		else {
			randomScale();
		}
	}

	void process(const ProcessArgs& args) override {
		std::lock_guard<std::mutex> lock(scaleMutex);
		const std::vector<float>& scl = scale;
		int n = (int) scl.size();
		std::vector<int> playing(n, 0);

		int channels = std::max(inputs[PITCH_INPUT].getChannels(), 1);
		for (int c = 0; c < channels; c++) {
			float cents = inputs[PITCH_INPUT].getVoltage(c) * 1200.f;
			int octave = (int) std::floor(cents / 1200.f);
			float pos = cents - octave * 1200.f;

			// Closest enabled interval within the octave
			int bestIdx = -1;
			float bestDist = 1e30f;
			for (int i = 0; i < n; i++) {
				if (!enabled[i])
					continue;
				float d = std::abs(scl[i] - pos);
				if (d < bestDist) {
					bestDist = d;
					bestIdx = i;
				}
			}

			if (bestIdx < 0) {
				// All tones disabled: pass the input through unquantized
				outputs[PITCH_OUTPUT].setVoltage(inputs[PITCH_INPUT].getVoltage(c), c);
			}
			else {
				playing[bestIdx] = 1;
				outputs[PITCH_OUTPUT].setVoltage((octave * 1200.f + scl[bestIdx]) / 1200.f, c);
			}
		}
		outputs[PITCH_OUTPUT].setChannels(channels);
		this->playing = playing;
	}

	std::string getDescription() const {
		if (!description.empty())
			return description;
		if (!currentFile.empty())
			return currentFile;
		return "SCL QNT";
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		json_t* historyJ = json_array();
		for (const std::string& file : history) {
			json_array_append_new(historyJ, json_string(file.c_str()));
		}
		json_object_set_new(rootJ, "history", historyJ);
		json_object_set_new(rootJ, "index", json_integer(index));

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		history.clear();
		index = -1;

		json_t* historyJ = json_object_get(rootJ, "history");
		if (historyJ && json_is_array(historyJ)) {
			size_t n = json_array_size(historyJ);
			for (size_t i = 0; i < n; i++) {
				json_t* fileJ = json_array_get(historyJ, i);
				if (fileJ && json_is_string(fileJ)) {
					std::string file = json_string_value(fileJ);
					if (system::isFile(system::join(sclDir, file)))
						history.push_back(file);
				}
			}
		}

		json_t* indexJ = json_object_get(rootJ, "index");
		if (indexJ)
			index = json_integer_value(indexJ);

		if (history.empty()) {
			randomScale();
			return;
		}
		index = clamp(index, 0, (int) history.size() - 1);
		loadScale(history[index]);
	}
};


/** Scale description, drawn vertically. */
struct SCLNameDisplay : LedDisplay {
	SCLRNG* module;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;

		std::string text = module ? module->getDescription() : "";
		if (text.empty())
			return;

		std::shared_ptr<Font> font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

		float bounds[4];
		nvgFontSize(args.vg, 10);
		nvgTextBounds(args.vg, 0, 0, text.c_str(), NULL, bounds);
		float length = bounds[2] - bounds[0];
		float maxLength = box.size.y - mm2px(2);
		float fontSize = 10;
		if (length > maxLength)
			fontSize = std::max(5.f, 10.f * maxLength / length);
		// Cap by the column width
		fontSize = std::min(fontSize, box.size.x / 1.2f);
		nvgFontSize(args.vg, fontSize);

		nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));

		// Anchored to the bottom of the column (the text reads upward)
		nvgSave(args.vg);
		nvgTranslate(args.vg, box.size.x / 2, box.size.y - mm2px(1));
		nvgRotate(args.vg, -M_PI / 2);
		nvgText(args.vg, 0.01f * box.size.y, 0, text.c_str(), NULL);
		nvgRestore(args.vg);
	}
};


/** One toggleable tone strip in the scale display. */
struct SCLToneButton : OpaqueWidget {
	SCLRNG* module;
	int index;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;

		Rect r = box.zeroPos();
		float radius = std::min(1.5f, box.size.y / 2);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, RECT_ARGS(r), radius);

		std::lock_guard<std::mutex> lock(module->scaleMutex);
		int n = (int) module->scale.size();
		if (index >= n)
			return;
		if (module->playing[index])
			nvgFillColor(args.vg, SCHEME_YELLOW);
		else if (module->enabled[index])
			nvgFillColor(args.vg, nvgRGB(0x7f, 0x6b, 0x0a));
		else
			nvgFillColor(args.vg, nvgRGB(0x40, 0x40, 0x40));
		nvgFill(args.vg);
	}

	void onDragStart(const event::DragStart& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			std::lock_guard<std::mutex> lock(module->scaleMutex);
			if (index < (int) module->enabled.size())
				module->enabled[index] = !module->enabled[index];
		}
		OpaqueWidget::onDragStart(e);
	}

	void onDragEnter(const event::DragEnter& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			SCLToneButton* origin = dynamic_cast<SCLToneButton*>(e.origin);
			if (origin) {
				std::lock_guard<std::mutex> lock(module->scaleMutex);
				if (index < (int) module->enabled.size() && origin->index < (int) module->enabled.size())
					module->enabled[index] = module->enabled[origin->index];
			}
		}
		OpaqueWidget::onDragEnter(e);
	}
};


/** Per-bar thickness: normal size, thinned only where tones overlap. */
static void computeBarLayout(const std::vector<float>& scl, float boxH, std::vector<float>& tops, std::vector<float>& thick) {
	float inset = 0.02f * boxH;
	float usable = boxH - 2 * inset;
	float barHDefault = mm2px(0.768);
	float minBarH = barHDefault * 0.5f;
	int n = (int) scl.size();
	tops.resize(n);
	thick.assign(n, barHDefault);
	for (int i = 0; i < n; i++) {
		tops[i] = inset + (scl[i] / 1200.f) * (usable - barHDefault);
	}
	for (int i = 0; i < n; i++) {
		float roomNext = (i < n - 1) ? (tops[i + 1] - tops[i]) : 1e9f;
		float roomPrev = (i > 0) ? (tops[i] - (tops[i - 1] + thick[i - 1])) : 1e9f;
		float t = std::min(barHDefault, std::min(roomNext, roomPrev));
		thick[i] = std::max(t, minBarH);
	}
}


/** Tone strips, positioned proportionally to their cents within the octave. */
struct SCLScaleDisplay : LedDisplay {
	SCLRNG* module;
	std::vector<SCLToneButton*> buttons;

	void rebuild(int n) {
		for (SCLToneButton* b : buttons) {
			removeChild(b);
			delete b;
		}
		buttons.clear();
		for (int i = 0; i < n; i++) {
			SCLToneButton* b = new SCLToneButton();
			b->module = module;
			b->index = i;
			addChild(b);
			buttons.push_back(b);
		}
	}

	void layout(const std::vector<float>& scl) {
		std::vector<float> tops, thick;
		computeBarLayout(scl, box.size.y, tops, thick);
		float padX = mm2px(0.85);
		for (int i = 0; i < (int) scl.size(); i++) {
			buttons[i]->box.pos = Vec(padX, tops[i]);
			buttons[i]->box.size = Vec(box.size.x - 2 * padX, thick[i]);
		}
	}

	void step() override {
		if (module) {
			std::vector<float> scl;
			{
				std::lock_guard<std::mutex> lock(module->scaleMutex);
				scl = module->scale;
			}
			if ((int) scl.size() != (int) buttons.size())
				rebuild((int) scl.size());
			layout(scl);
		}
		LedDisplay::step();
	}
};


/** Cents values of the current scale, listed vertically. */
struct SCLValuesDisplay : LedDisplay {
	SCLRNG* module;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1 || !module)
			return;

		std::vector<float> scl;
		std::vector<int> enabled;
		{
			std::lock_guard<std::mutex> lock(module->scaleMutex);
			scl = module->scale;
			enabled = module->enabled;
		}
		int n = (int) scl.size();
		if (n == 0)
			return;

		std::shared_ptr<Font> font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

		float h = box.size.y;
		float xOffset = mm2px(0.5) + 0.1f * box.size.x;

		float fontSize = 5.44f;
		nvgFontSize(args.vg, fontSize);

		// Shrink so the widest value fits the column
		float maxW = 0.f;
		for (int i = 0; i < n; i++) {
			std::string s = string::f("%.2f", scl[i]);
			float bounds[4];
			nvgTextBounds(args.vg, 0, 0, s.c_str(), NULL, bounds);
			maxW = std::max(maxW, bounds[2] - bounds[0]);
		}
		float maxTextW = box.size.x - xOffset;
		if (maxW > maxTextW)
			fontSize = std::max(2.72f, fontSize * maxTextW / maxW);
		nvgFontSize(args.vg, fontSize);

		// Bar-aligned label centers
		std::vector<float> tops, thick;
		computeBarLayout(scl, h, tops, thick);
		std::vector<float> y(n);
		for (int i = 0; i < n; i++) {
			y[i] = tops[i] + thick[i] / 2;
		}

		// Spread overlapping labels apart; if they still do not fit, shrink the font
		while (true) {
			float labelH = fontSize * 0.8f;
			std::vector<float> yy = y;
			for (int i = 1; i < n; i++) {
				float minY = yy[i - 1] + labelH;
				if (yy[i] < minY)
					yy[i] = minY;
			}
			if (yy[n - 1] + labelH / 2 <= h || fontSize <= 2.f) {
				y = yy;
				break;
			}
			fontSize *= 0.85f;
			nvgFontSize(args.vg, fontSize);
		}

		for (int i = 0; i < n; i++) {
			if (i < (int) enabled.size() && !enabled[i])
				nvgFillColor(args.vg, nvgRGB(0x40, 0x40, 0x40));
			else
				nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
			std::string s = string::f("%.2f", scl[i]);
			nvgText(args.vg, xOffset, y[i], s.c_str(), NULL);
		}
	}
};


/** Decorative dice face between the navigation arrows. */
struct SCLDice : Widget {
	void draw(const DrawArgs& args) override {
		NVGcolor color = nvgRGB(0x8c, 0x8c, 0x8c);

		// Face outline
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, mm2px(0.5), mm2px(0.5), box.size.x - mm2px(1), box.size.y - mm2px(1), mm2px(0.8));
		nvgStrokeColor(args.vg, color);
		nvgStrokeWidth(args.vg, 1.5);
		nvgStroke(args.vg);

		// Pips
		Vec c = box.size.div(2);
		float pipR = box.size.x * 0.09f;
		float off = box.size.x * 0.28f;
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, c.x, c.y, pipR);
		nvgCircle(args.vg, c.x - off, c.y - off, pipR);
		nvgCircle(args.vg, c.x + off, c.y - off, pipR);
		nvgCircle(args.vg, c.x - off, c.y + off, pipR);
		nvgCircle(args.vg, c.x + off, c.y + off, pipR);
		nvgFillColor(args.vg, color);
		nvgFill(args.vg);
	}
};


/** Left/right arrow of the scale navigation. */
struct SCLArrowButton : OpaqueWidget {
	SCLRNG* module;
	bool left = false;

	void draw(const DrawArgs& args) override {
		NVGcolor color = settings::preferDarkPanels ? nvgRGB(0xeb, 0xeb, 0xeb) : nvgRGB(0x1f, 0x1f, 0x1f);

		nvgBeginPath(args.vg);
		Vec c = box.size.div(2);
		float s = std::min(box.size.x, box.size.y) * 0.22f;
		if (left) {
			nvgMoveTo(args.vg, c.x - s, c.y);
			nvgLineTo(args.vg, c.x + s, c.y - s);
			nvgLineTo(args.vg, c.x + s, c.y + s);
		}
		else {
			nvgMoveTo(args.vg, c.x + s, c.y);
			nvgLineTo(args.vg, c.x - s, c.y - s);
			nvgLineTo(args.vg, c.x - s, c.y + s);
		}
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, color);
		nvgFill(args.vg);
	}

	void onDragStart(const event::DragStart& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (left)
				module->prevScale();
			else
				module->nextScale();
		}
		OpaqueWidget::onDragStart(e);
	}
};


struct SCLRNGWidget : ModuleWidget {
	SCLRNGWidget(SCLRNG* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SCLRNG.svg"), asset::plugin(pluginInstance, "res/SCLRNG-dark.svg")));

		addChild(createWidget<ThemedScrew>(Vec(0, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		NVGcolor labelColor = settings::preferDarkPanels ? nvgRGB(0xeb, 0xeb, 0xeb) : nvgRGB(0x1f, 0x1f, 0x1f);

		// Title
		PanelLabel* title = createWidget<PanelLabel>(mm2px(Vec(0, 4)));
		title->box.size = mm2px(Vec(20.32, 7));
		title->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Bold.ttf");
		title->text = "SCL QNT";
		title->fontSize = 13;
		title->color = labelColor;
		addChild(title);

		// Scale navigation: [<] [dice] [>]
		SCLArrowButton* leftButton = createWidget<SCLArrowButton>(mm2px(Vec(0.36, 12.4)));
		leftButton->box.size = mm2px(Vec(6.4, 6.4));
		leftButton->module = module;
		leftButton->left = true;
		addChild(leftButton);

		SCLDice* dice = createWidget<SCLDice>(mm2px(Vec(6.96, 12.4)));
		dice->box.size = mm2px(Vec(6.4, 6.4));
		addChild(dice);

		SCLArrowButton* rightButton = createWidget<SCLArrowButton>(mm2px(Vec(13.56, 12.4)));
		rightButton->box.size = mm2px(Vec(6.4, 6.4));
		rightButton->module = module;
		rightButton->left = false;
		addChild(rightButton);

		// Scale name (left 1/5)
		SCLNameDisplay* nameDisplay = createWidget<SCLNameDisplay>(mm2px(Vec(1.625, 21.5)));
		nameDisplay->box.size = mm2px(Vec(3.414, 84));
		nameDisplay->module = module;
		addChild(nameDisplay);

		// Tone strips (middle 2/5)
		SCLScaleDisplay* scaleDisplay = createWidget<SCLScaleDisplay>(mm2px(Vec(5.039, 21.5)));
		scaleDisplay->box.size = mm2px(Vec(6.828, 84));
		scaleDisplay->module = module;
		addChild(scaleDisplay);

		// Cents values (right 2/5)
		SCLValuesDisplay* valuesDisplay = createWidget<SCLValuesDisplay>(mm2px(Vec(11.867, 21.5)));
		valuesDisplay->box.size = mm2px(Vec(6.828, 84));
		valuesDisplay->module = module;
		addChild(valuesDisplay);

		// Jacks
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.08, 112.5)), module, SCLRNG::PITCH_INPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(15.24, 112.5)), module, SCLRNG::PITCH_OUTPUT));

		// Jack labels below the ports
		PanelLabel* inLabel = createWidget<PanelLabel>(mm2px(Vec(1.08, 117.6)));
		inLabel->box.size = mm2px(Vec(8, 3.5));
		inLabel->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf");
		inLabel->text = "IN";
		inLabel->fontSize = 7;
		inLabel->color = labelColor;
		addChild(inLabel);

		PanelLabel* outLabel = createWidget<PanelLabel>(mm2px(Vec(11.24, 117.6)));
		outLabel->box.size = mm2px(Vec(8, 3.5));
		outLabel->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf");
		outLabel->text = "OUT";
		outLabel->fontSize = 7;
		outLabel->color = labelColor;
		addChild(outLabel);
	}

	void appendContextMenu(Menu* menu) override {
		SCLRNG* module = getModule<SCLRNG>();

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Random scale", "", [=]() {
			module->randomScale();
		}));
		menu->addChild(createMenuItem("Choose scala file", "", [=]() {
			char* path = osdialog_file(OSDIALOG_OPEN, module->sclDir.c_str(), NULL, NULL);
			if (path) {
				std::string chosen(path);
				std::free(path);
				module->chooseScale(chosen);
			}
		}));
	}
};


Model* modelSCLRNG = createModel<SCLRNG, SCLRNGWidget>("SCLRNG");
