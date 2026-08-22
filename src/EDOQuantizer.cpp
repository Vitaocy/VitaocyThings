#include "plugin.hpp"

#include <atomic>


struct EDOQuantizer : Module {
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

	static constexpr int MIN_EDO = 1;
	static constexpr int MAX_EDO = 48;

	int edo = 12;
	bool enabledNotes[MAX_EDO] = {};
	// Intervals [i / (2*edo), (i + 1) / (2*edo)) V mapping to the closest enabled note
	std::atomic<int> ranges[2 * MAX_EDO] = {};
	// Written on the audio thread, read by the UI thread
	std::atomic<bool> playingNotes[MAX_EDO] = {};

	EDOQuantizer() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configInput(PITCH_INPUT, "1V/octave pitch");
		configOutput(PITCH_OUTPUT, "Pitch");
		configBypass(PITCH_INPUT, PITCH_OUTPUT);

		onReset();
	}

	void onReset() override {
		for (int i = 0; i < MAX_EDO; i++) {
			enabledNotes[i] = true;
		}
		updateRanges();
	}

	void onRandomize() override {
		for (int i = 0; i < MAX_EDO; i++) {
			enabledNotes[i] = (random::uniform() < 0.5f);
		}
		updateRanges();
	}

	void setEdo(int edo) {
		this->edo = clamp(edo, MIN_EDO, MAX_EDO);
		updateRanges();
	}

	void incrementEdo() {
		setEdo(edo + 1);
	}

	void decrementEdo() {
		setEdo(edo - 1);
	}

	void process(const ProcessArgs& args) override {
		bool playing[MAX_EDO] = {};
		int channels = std::max(inputs[PITCH_INPUT].getChannels(), 1);
		int segments = 2 * edo;

		for (int c = 0; c < channels; c++) {
			float pitch = inputs[PITCH_INPUT].getVoltage(c);

			// Split pitch into octave and position within the octave
			int range = std::floor(pitch * segments);
			int octave = eucDiv(range, segments);
			range -= octave * segments;

			int note = ranges[range] + octave * edo;
			playing[eucMod(note, edo)] = true;
			pitch = float(note) / edo;
			outputs[PITCH_OUTPUT].setVoltage(pitch, c);
		}
		outputs[PITCH_OUTPUT].setChannels(channels);
		for (int i = 0; i < MAX_EDO; i++) {
			playingNotes[i].store(playing[i]);
		}
	}

	/** Maps each half-step voltage range to the closest enabled note. */
	void updateRanges() {
		// Check if no notes are enabled
		bool anyEnabled = false;
		for (int i = 0; i < edo; i++) {
			if (enabledNotes[i]) {
				anyEnabled = true;
				break;
			}
		}

		int segments = 2 * edo;
		for (int i = 0; i < segments; i++) {
			int closestNote = 0;
			int closestDist = INT_MAX;
			// Search one octave below and above so ranges wrap around smoothly
			for (int note = -edo; note < 2 * edo; note++) {
				int dist = std::abs((i + 1) / 2 - note);
				// Ignore enabled state if no notes are enabled
				if (anyEnabled && !enabledNotes[eucMod(note, edo)]) {
					continue;
				}
				if (dist < closestDist) {
					closestNote = note;
					closestDist = dist;
				}
				else {
					// If dist increases, we won't find a better one.
					break;
				}
			}
			ranges[i] = closestNote;
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		json_object_set_new(rootJ, "edo", json_integer(edo));

		json_t* enabledNotesJ = json_array();
		for (int i = 0; i < MAX_EDO; i++) {
			json_array_insert_new(enabledNotesJ, i, json_boolean(enabledNotes[i]));
		}
		json_object_set_new(rootJ, "enabledNotes", enabledNotesJ);

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* edoJ = json_object_get(rootJ, "edo");
		if (edoJ) {
			setEdo(json_integer_value(edoJ));
		}

		json_t* enabledNotesJ = json_object_get(rootJ, "enabledNotes");
		if (enabledNotesJ) {
			for (int i = 0; i < MAX_EDO; i++) {
				json_t* enabledNoteJ = json_array_get(enabledNotesJ, i);
				if (enabledNoteJ) {
					enabledNotes[i] = json_boolean_value(enabledNoteJ);
				}
			}
		}
		updateRanges();
	}
};


/** One toggleable tone in the scale display. */
struct EDOQuantizerButton : OpaqueWidget {
	EDOQuantizer* module;
	int note;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;

		Rect r = box.zeroPos();
		float radius = std::min(1.5f, box.size.x / 2);

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, RECT_ARGS(r), radius);
		if (module ? module->playingNotes[note].load() : (note == 0)) {
			nvgFillColor(args.vg, SCHEME_YELLOW);
		}
		else if (module ? module->enabledNotes[note] : true) {
			nvgFillColor(args.vg, nvgRGB(0x7f, 0x6b, 0x0a));
		}
		else {
			nvgFillColor(args.vg, nvgRGB(0x40, 0x40, 0x40));
		}
		nvgFill(args.vg);
	}

	void onDragStart(const event::DragStart& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			module->enabledNotes[note] = !module->enabledNotes[note];
			module->updateRanges();
		}
		OpaqueWidget::onDragStart(e);
	}

	void onDragEnter(const event::DragEnter& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			EDOQuantizerButton* origin = dynamic_cast<EDOQuantizerButton*>(e.origin);
			if (origin) {
				module->enabledNotes[note] = module->enabledNotes[origin->note];
				module->updateRanges();
			}
		}
		OpaqueWidget::onDragEnter(e);
	}
};


/** Stacked tone strips, one per note in the octave. */
struct EDOQuantizerDisplay : LedDisplay {
	EDOQuantizer* module;
	EDOQuantizerButton* buttons[EDOQuantizer::MAX_EDO] = {};
	int lastEdo = 0;

	EDOQuantizerDisplay() {
		for (int i = 0; i < EDOQuantizer::MAX_EDO; i++) {
			EDOQuantizerButton* button = new EDOQuantizerButton();
			button->note = i;
			button->setVisible(false);
			addChild(button);
			buttons[i] = button;
		}
	}

	void setModule(EDOQuantizer* module) {
		this->module = module;
		for (int i = 0; i < EDOQuantizer::MAX_EDO; i++) {
			buttons[i]->module = module;
		}
	}

	void step() override {
		if (module && module->edo != lastEdo) {
			lastEdo = module->edo;
			updateButtonGeometry();
		}
		LedDisplay::step();
	}

	void updateButtonGeometry() {
		int n = module->edo;
		const float padX = mm2px(2.0f);
		const float padY = mm2px(2.5f);
		const float gap = mm2px(0.4f);

		float w = box.size.x - 2 * padX;
		float h = box.size.y - 2 * padY;
		float barH = (h - (n - 1) * gap) / n;

		for (int i = 0; i < EDOQuantizer::MAX_EDO; i++) {
			EDOQuantizerButton* button = buttons[i];
			bool visible = (i < n);
			if (button->isVisible() != visible)
				button->setVisible(visible);
			if (visible) {
				// Full-width strips stacked vertically, lowest note at the bottom
				float y = padY + (n - 1 - i) * (barH + gap);
				button->box.pos = Vec(padX, y);
				button->box.size = Vec(w, barH);
			}
		}
	}
};


/** 7-segment display for the current EDO number. */
struct EDOQuantizerCounterDisplay : DigitalDisplay {
	EDOQuantizer* module;

	EDOQuantizerCounterDisplay() {
		fontPath = asset::system("res/fonts/DSEG7ClassicMini-BoldItalic.ttf");
		textPos = Vec(11.8, 16.0); // centered in the 8 x 6 mm window
		bgText = "48";
		fontSize = 14.4;
		align = NVG_ALIGN_CENTER;
	}

	void step() override {
		text = string::f("%d", module ? module->edo : 12);
		DigitalDisplay::step();
	}
};


/** Up/down arrow button of the EDO counter. Repeats while held. */
struct EDOArrowButton : OpaqueWidget {
	EDOQuantizer* module;
	bool up = false;

	bool held = false;
	dsp::Timer repeatTimer;
	float repeatDelay = 0.4f;

	void draw(const DrawArgs& args) override {
		NVGcolor color = settings::preferDarkPanels ? nvgRGB(0xeb, 0xeb, 0xeb) : nvgRGB(0x1f, 0x1f, 0x1f);

		nvgBeginPath(args.vg);
		Vec c = box.size.div(2);
		float s = std::min(box.size.x, box.size.y) * 0.22f;
		if (up) {
			nvgMoveTo(args.vg, c.x, c.y - s);
			nvgLineTo(args.vg, c.x - s, c.y + s);
			nvgLineTo(args.vg, c.x + s, c.y + s);
		}
		else {
			nvgMoveTo(args.vg, c.x - s, c.y - s);
			nvgLineTo(args.vg, c.x + s, c.y - s);
			nvgLineTo(args.vg, c.x, c.y + s);
		}
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, color);
		nvgFill(args.vg);
	}

	void onDragStart(const event::DragStart& e) override {
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			held = true;
			repeatTimer.reset();
			repeatDelay = 0.4f;
			change();
		}
		OpaqueWidget::onDragStart(e);
	}

	void onDragEnd(const event::DragEnd& e) override {
		held = false;
		OpaqueWidget::onDragEnd(e);
	}

	void step() override {
		if (held) {
			repeatTimer.process((float) APP->window->getLastFrameDuration());
			if (repeatTimer.getTime() >= repeatDelay) {
				repeatTimer.reset();
				repeatDelay = 0.08f;
				change();
			}
		}
		OpaqueWidget::step();
	}

	void change() {
		if (up)
			module->incrementEdo();
		else
			module->decrementEdo();
	}
};


/** Panel silkscreen text, drawn with a font instead of SVG paths. */
struct EDOQuantizerLabel : Widget {
	std::string text;
	std::string fontPath = asset::system("res/fonts/ShareTechMono-Regular.ttf");
	NVGcolor color = nvgRGB(0x1f, 0x1f, 0x1f);
	float fontSize = 10;

	void draw(const DrawArgs& args) override {
		std::shared_ptr<Font> font = APP->window->loadFont(fontPath);
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, fontSize);
		nvgTextLetterSpacing(args.vg, 1);
		nvgFillColor(args.vg, color);
		nvgTextAlign(args.vg, NVG_ALIGN_MIDDLE | NVG_ALIGN_CENTER);
		nvgText(args.vg, box.size.x / 2, box.size.y / 2, text.c_str(), NULL);
	}
};


struct EDOQuantizerWidget : ModuleWidget {
	EDOQuantizerWidget(EDOQuantizer* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/EDOQuantizer.svg"), asset::plugin(pluginInstance, "res/EDOQuantizer-dark.svg")));

		addChild(createWidget<ThemedScrew>(Vec(0, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		NVGcolor labelColor = settings::preferDarkPanels ? nvgRGB(0xeb, 0xeb, 0xeb) : nvgRGB(0x1f, 0x1f, 0x1f);

		// Title
		EDOQuantizerLabel* title = createWidget<EDOQuantizerLabel>(mm2px(Vec(0, 4)));
		title->box.size = mm2px(Vec(20.32, 7));
		title->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Bold.ttf");
		title->text = "EDO QNT";
		title->fontSize = 13.2;
		title->color = labelColor;
		addChild(title);

		// EDO counter: [down arrow] [number] [up arrow]
		EDOArrowButton* downButton = createWidget<EDOArrowButton>(mm2px(Vec(1.16, 13.1)));
		downButton->box.size = mm2px(Vec(4, 4));
		downButton->module = module;
		downButton->up = false;
		addChild(downButton);

		EDOQuantizerCounterDisplay* counterDisplay = createWidget<EDOQuantizerCounterDisplay>(mm2px(Vec(6.16, 12.1)));
		counterDisplay->box.size = mm2px(Vec(8, 6));
		counterDisplay->module = module;
		addChild(counterDisplay);

		EDOArrowButton* upButton = createWidget<EDOArrowButton>(mm2px(Vec(15.16, 13.1)));
		upButton->box.size = mm2px(Vec(4, 4));
		upButton->module = module;
		upButton->up = true;
		addChild(upButton);

		// Scale display with toggleable tones
		EDOQuantizerDisplay* display = createWidget<EDOQuantizerDisplay>(mm2px(Vec(3.048, 21.5)));
		display->box.size = mm2px(Vec(14.224, 84));
		display->setModule(module);
		addChild(display);

		// Jacks
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.08, 112.5)), module, EDOQuantizer::PITCH_INPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(15.24, 112.5)), module, EDOQuantizer::PITCH_OUTPUT));

		// Jack labels below the ports
		EDOQuantizerLabel* inLabel = createWidget<EDOQuantizerLabel>(mm2px(Vec(1.08, 117.6)));
		inLabel->box.size = mm2px(Vec(8, 3.5));
		inLabel->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf");
		inLabel->text = "IN";
		inLabel->fontSize = 7;
		inLabel->color = labelColor;
		addChild(inLabel);

		EDOQuantizerLabel* outLabel = createWidget<EDOQuantizerLabel>(mm2px(Vec(11.24, 117.6)));
		outLabel->box.size = mm2px(Vec(8, 3.5));
		outLabel->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf");
		outLabel->text = "OUT";
		outLabel->fontSize = 7;
		outLabel->color = labelColor;
		addChild(outLabel);
	}

	void appendContextMenu(Menu* menu) override {
		EDOQuantizer* module = getModule<EDOQuantizer>();

		menu->addChild(new MenuSeparator);

		menu->addChild(createMenuItem("Enable all notes", "", [=]() {
			for (int i = 0; i < EDOQuantizer::MAX_EDO; i++)
				module->enabledNotes[i] = true;
			module->updateRanges();
		}));
		menu->addChild(createMenuItem("Disable all notes", "", [=]() {
			for (int i = 0; i < EDOQuantizer::MAX_EDO; i++)
				module->enabledNotes[i] = false;
			module->updateRanges();
		}));
	}
};


Model* modelEDOQuantizer = createModel<EDOQuantizer, EDOQuantizerWidget>("EDOQuantizer");
