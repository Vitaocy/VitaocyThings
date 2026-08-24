#include "plugin.hpp"

#include <cmath>


/** Glide time knob: logarithmic scale so short times are finely controllable. */
struct GlideTimeQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float t = 0.01f * std::pow(10.f, getValue() * 2.699f);
		return string::f("%.3g", t);
	}
};


/** Pitch glider: on each detected pitch change, glides from the previous pitch
to the new one over the knob-set time, with a knob-shaped curve and a chance
that the glide triggers at all. Each knob has a CV input and a CV amount knob
that scales how strongly the CV affects the knob. */
struct VOGLI : Module {
	enum ParamIds {
		TIME_PARAM,
		CURVE_PARAM,
		CHANCE_PARAM,
		TIME_AMOUNT_PARAM,
		CURVE_AMOUNT_PARAM,
		CHANCE_AMOUNT_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		PITCH_INPUT,
		CURVE_CV_INPUT,
		TIME_CV_INPUT,
		CHANCE_CV_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		PITCH_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		NUM_LIGHTS
	};

	static constexpr int MAX_POLY = 16;
	static constexpr float DETECT_V = 0.01f;
	static constexpr float TIME_MIN = 0.01f;
	static constexpr float TIME_MAX = 5.f;
	// A 10 V CV signal sweeps the full knob range at full amount
	static constexpr float CV_DEPTH = 0.1f;

	// Effective knob values with the CV amount (not the live signal), for the knob arc
	float timeBase = 0.5f;
	float curveBase = 0.5f;
	float chanceBase = 1.f;

	float lastPitch[MAX_POLY] = {};
	float glideStart[MAX_POLY] = {};
	float glideTarget[MAX_POLY] = {};
	float glidePos[MAX_POLY] = {};
	bool gliding[MAX_POLY] = {};

	VOGLI() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam<GlideTimeQuantity>(TIME_PARAM, 0.f, 1.f, 0.5f, "Glide time", " s");
		configParam(CURVE_PARAM, 0.f, 1.f, 0.5f, "Glide curve");
		configParam(CHANCE_PARAM, 0.f, 1.f, 1.f, "Glide chance", "%", 0.f, 100.f);
		configParam(TIME_AMOUNT_PARAM, 0.f, 1.f, 0.f, "Time CV amount");
		configParam(CURVE_AMOUNT_PARAM, 0.f, 1.f, 0.f, "Curve CV amount");
		configParam(CHANCE_AMOUNT_PARAM, 0.f, 1.f, 0.f, "Chance CV amount");
		configInput(PITCH_INPUT, "1V/octave pitch");
		configInput(CURVE_CV_INPUT, "Curve CV");
		configInput(TIME_CV_INPUT, "Time CV");
		configInput(CHANCE_CV_INPUT, "Chance CV");
		configOutput(PITCH_OUTPUT, "Pitch");
		configBypass(PITCH_INPUT, PITCH_OUTPUT);
	}

	/** Effective curve value with live CV for channel 0, used by the curve preview. */
	float getCurve() {
		return clamp(curveBase + inputs[CURVE_CV_INPUT].getVoltage() * params[CURVE_AMOUNT_PARAM].getValue() * CV_DEPTH, 0.f, 1.f);
	}

	/** CV contribution (after the amount knob) for a control, channel 0. */
	float cvIn(InputIds input, ParamIds amount) {
		return inputs[input].getVoltage() * params[amount].getValue() * CV_DEPTH;
	}

	void process(const ProcessArgs& args) override {
		// Refresh the knob base values (params are only changed by the UI thread)
		timeBase = params[TIME_PARAM].getValue();
		curveBase = params[CURVE_PARAM].getValue();
		chanceBase = params[CHANCE_PARAM].getValue();

		int channels = std::max(inputs[PITCH_INPUT].getChannels(), 1);

		for (int c = 0; c < channels; c++) {
			float pitch = inputs[PITCH_INPUT].getVoltage(c);

			// Knob values, externally modulatable via CV scaled by the amount knob
			float timeParam = clamp(timeBase + inputs[TIME_CV_INPUT].getPolyVoltage(c) * params[TIME_AMOUNT_PARAM].getValue() * CV_DEPTH, 0.f, 1.f);
			float curveParam = clamp(curveBase + inputs[CURVE_CV_INPUT].getPolyVoltage(c) * params[CURVE_AMOUNT_PARAM].getValue() * CV_DEPTH, 0.f, 1.f);
			float chance = clamp(chanceBase + inputs[CHANCE_CV_INPUT].getPolyVoltage(c) * params[CHANCE_AMOUNT_PARAM].getValue() * CV_DEPTH, 0.f, 1.f);

			float time = TIME_MIN * std::pow(10.f, timeParam * std::log10(TIME_MAX / TIME_MIN));
			// Curve exponent: 0.1 = exponential (fast start), 1 = linear, 10 = logarithmic
			float k = std::pow(10.f, 2.f * (0.5f - curveParam));

			// Detect a new pitch
			if (std::abs(pitch - lastPitch[c]) > DETECT_V) {
				lastPitch[c] = pitch;
				if (random::uniform() < chance) {
					glideStart[c] = outputs[PITCH_OUTPUT].getVoltage(c);
					glideTarget[c] = pitch;
					glidePos[c] = 0.f;
					gliding[c] = true;
				}
				else {
					gliding[c] = false;
				}
			}

			if (gliding[c]) {
				glidePos[c] += args.sampleTime / time;
				float p = std::min(glidePos[c], 1.f);
				float shaped = std::pow(p, k);
				outputs[PITCH_OUTPUT].setVoltage(glideStart[c] + (glideTarget[c] - glideStart[c]) * shaped, c);
				if (p >= 1.f)
					gliding[c] = false;
			}
			else {
				outputs[PITCH_OUTPUT].setVoltage(pitch, c);
			}
		}
		outputs[PITCH_OUTPUT].setChannels(channels);
	}
};


/** Glide curve preview: shows the shape of the current curve and its character.
The line turns yellow the further the curve is from the linear position. */
struct VOGLICurveDisplay : LedDisplay {
	VOGLI* module;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1 || !module)
			return;

		float curve = module->getCurve();
		float k = std::pow(10.f, 2.f * (0.5f - curve));

		float w = box.size.x;
		float h = box.size.y;
		float padX = mm2px(2);
		float curveH = h - mm2px(6);

		// Color: white at linear, yellow (#ffd714) at the extremes
		float t = std::min(1.f, std::abs(curve - 0.5f) * 2.f);
		NVGcolor lineColor = nvgRGB(
			(uint8_t)(0xff + (0xff - 0xff) * t),
			(uint8_t)(0xff + (0xd7 - 0xff) * t),
			(uint8_t)(0xff + (0x14 - 0xff) * t));

		// Curve: time on x, pitch progress on y
		nvgBeginPath(args.vg);
		int segs = 40;
		for (int i = 0; i <= segs; i++) {
			float p = (float) i / segs;
			float shaped = std::pow(p, k);
			float x = padX + p * (w - 2 * padX);
			float y = mm2px(1) + 0.075f * h + (1.f - shaped) * curveH;
			if (i == 0)
				nvgMoveTo(args.vg, x, y);
			else
				nvgLineTo(args.vg, x, y);
		}
		nvgStrokeColor(args.vg, lineColor);
		nvgStrokeWidth(args.vg, 1.65);
		nvgStroke(args.vg);

		// Character label
		const char* label = (k > 1.1f) ? "LOG" : (k < 0.9f) ? "EXP" : "LIN";
		std::shared_ptr<Font> font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 6);
		nvgTextLetterSpacing(args.vg, 1);
		nvgFillColor(args.vg, nvgRGB(0x8c, 0x8c, 0x8c));
		nvgTextAlign(args.vg, NVG_ALIGN_MIDDLE | NVG_ALIGN_CENTER);
		nvgText(args.vg, w / 2, h - mm2px(2.5), label, NULL);
	}
};


/** Value (0..1) to screen angle for drawing arcs along the knob travel. */
static float knobAngle(float value, float minAngle, float maxAngle) {
	return -M_PI / 2 + (minAngle + value * (maxAngle - minAngle));
}

/** Draws a circular arc segment as a polyline. */
static void drawArc(NVGcontext* vg, Vec c, float r, float a0, float a1) {
	nvgBeginPath(vg);
	int segs = 24;
	for (int i = 0; i <= segs; i++) {
		float a = a0 + (a1 - a0) * i / segs;
		float x = c.x + std::cos(a) * r;
		float y = c.y + std::sin(a) * r;
		if (i == 0)
			nvgMoveTo(vg, x, y);
		else
			nvgLineTo(vg, x, y);
	}
	nvgStroke(vg);
}

/** Main knob with a CV range arc: a gray arc shows how far CV can push the knob
past its current position, and a yellow arc shows the live CV offset. The arcs
are drawn on the knob face at 10% transparency so the knob stays readable. */
struct CVArcKnob : RoundSmallBlackKnob {
	VOGLI* module;
	VOGLI::InputIds cvInput;
	VOGLI::ParamIds amountParam;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1 || !module)
			return;

		// Current knob position
		float base = 0.f;
		if (paramId == VOGLI::TIME_PARAM) base = module->timeBase;
		else if (paramId == VOGLI::CURVE_PARAM) base = module->curveBase;
		else if (paramId == VOGLI::CHANCE_PARAM) base = module->chanceBase;
		base = clamp(base, 0.f, 1.f);

		Vec c = box.size.div(2);
		float r = box.size.x / 2 + 2.f;

		// Gray: the reachable range set by the CV amount knob, independent of the signal.
		// A full-scale (10 V) CV signal sweeps the whole knob, so the bounds are
		// simply the amount knob's position around the current knob value.
		float reach = module->params[amountParam].getValue();
		if (reach > 0.001f) {
			float lo = clamp(base - reach, 0.f, 1.f);
			float hi = clamp(base + reach, 0.f, 1.f);
			nvgStrokeWidth(args.vg, 1.8f);
			nvgStrokeColor(args.vg, nvgRGBA(0x40, 0x40, 0x40, 230));
			drawArc(args.vg, c, r, knobAngle(lo, minAngle, maxAngle), knobAngle(hi, minAngle, maxAngle));
		}

		// Yellow: where the live CV signal currently sits
		float cv = module->cvIn(cvInput, amountParam);
		if (std::abs(cv) > 0.001f) {
			float live = clamp(base + cv, 0.f, 1.f);
			nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xd7, 0x14, 230));
			drawArc(args.vg, c, r, knobAngle(std::min(base, live), minAngle, maxAngle), knobAngle(std::max(base, live), minAngle, maxAngle));
		}
	}
};


struct VOGLIWidget : ModuleWidget {
	VOGLIWidget(VOGLI* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VOGLI.svg"), asset::plugin(pluginInstance, "res/VOGLI-dark.svg")));

		addChild(createWidget<ThemedScrew>(Vec(0, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		NVGcolor labelColor = settings::preferDarkPanels ? nvgRGB(0xeb, 0xeb, 0xeb) : nvgRGB(0x1f, 0x1f, 0x1f);
		NVGcolor dimColor = settings::preferDarkPanels ? nvgRGB(0x8c, 0x8c, 0x8c) : nvgRGB(0x66, 0x66, 0x66);

		// Title
		PanelLabel* title = createWidget<PanelLabel>(mm2px(Vec(0, 4)));
		title->box.size = mm2px(Vec(20.32, 7));
		title->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Bold.ttf");
		title->text = "V/O GLI";
		title->fontSize = 13;
		title->color = labelColor;
		addChild(title);

		// Curve preview, right above the CURVE group
		VOGLICurveDisplay* display = createWidget<VOGLICurveDisplay>(mm2px(Vec(3.048, 12)));
		display->box.size = mm2px(Vec(14.224, 11));
		display->module = module;
		addChild(display);

		// CURVE group, triangle pointing right
		PanelLabel* curveLabel = createWidget<PanelLabel>(mm2px(Vec(0, 27.65)));
		curveLabel->box.size = mm2px(Vec(20.32, 3));
		curveLabel->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf");
		curveLabel->text = "CURVE";
		curveLabel->fontSize = 5;
		curveLabel->color = dimColor;
		addChild(curveLabel);

		{
			CVArcKnob* k = createParamCentered<CVArcKnob>(mm2px(Vec(7.56, 36.5)), module, VOGLI::CURVE_PARAM);
			k->module = module;
			k->cvInput = VOGLI::CURVE_CV_INPUT;
			k->amountParam = VOGLI::CURVE_AMOUNT_PARAM;
			addParam(k);
		}
		addParam(createParamCentered<Trimpot>(mm2px(Vec(15.35, 41)), module, VOGLI::CURVE_AMOUNT_PARAM));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(7.56, 45.5)), module, VOGLI::CURVE_CV_INPUT));

		// TIME group, mirrored (triangle pointing left)
		PanelLabel* timeLabel = createWidget<PanelLabel>(mm2px(Vec(0, 54.65)));
		timeLabel->box.size = mm2px(Vec(20.32, 3));
		timeLabel->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf");
		timeLabel->text = "TIME";
		timeLabel->fontSize = 5;
		timeLabel->color = dimColor;
		addChild(timeLabel);

		{
			CVArcKnob* k = createParamCentered<CVArcKnob>(mm2px(Vec(12.76, 63.5)), module, VOGLI::TIME_PARAM);
			k->module = module;
			k->cvInput = VOGLI::TIME_CV_INPUT;
			k->amountParam = VOGLI::TIME_AMOUNT_PARAM;
			addParam(k);
		}
		addParam(createParamCentered<Trimpot>(mm2px(Vec(4.97, 68)), module, VOGLI::TIME_AMOUNT_PARAM));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(12.76, 72.5)), module, VOGLI::TIME_CV_INPUT));

		// CHANCE group, triangle pointing right
		PanelLabel* chanceLabel = createWidget<PanelLabel>(mm2px(Vec(0, 81.65)));
		chanceLabel->box.size = mm2px(Vec(20.32, 3));
		chanceLabel->fontPath = asset::plugin(pluginInstance, "res/fonts/RobotoCondensed-Regular.ttf");
		chanceLabel->text = "CHANCE";
		chanceLabel->fontSize = 5;
		chanceLabel->color = dimColor;
		addChild(chanceLabel);

		{
			CVArcKnob* k = createParamCentered<CVArcKnob>(mm2px(Vec(7.56, 90.5)), module, VOGLI::CHANCE_PARAM);
			k->module = module;
			k->cvInput = VOGLI::CHANCE_CV_INPUT;
			k->amountParam = VOGLI::CHANCE_AMOUNT_PARAM;
			addParam(k);
		}
		addParam(createParamCentered<Trimpot>(mm2px(Vec(15.35, 95)), module, VOGLI::CHANCE_AMOUNT_PARAM));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(7.56, 99.5)), module, VOGLI::CHANCE_CV_INPUT));

		// Jacks at the bottom
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.08, 112.5)), module, VOGLI::PITCH_INPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(15.24, 112.5)), module, VOGLI::PITCH_OUTPUT));

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
};


Model* modelVOGLI = createModel<VOGLI, VOGLIWidget>("VOGLI");
