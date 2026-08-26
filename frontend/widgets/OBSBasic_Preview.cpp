/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <utility/display-helpers.hpp>
#include <widgets/OBSProjector.hpp>

#include <qt-wrappers.hpp>

#include <QColorDialog>

#include <algorithm>
#include <sstream>

extern void undo_redo(const std::string &data);

using namespace std;

void OBSBasic::InitPrimitives()
{
	ProfileScope("OBSBasic::InitPrimitives");

	obs_enter_graphics();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	box = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	boxLeft = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	boxTop = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	boxRight = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	boxBottom = gs_render_save();

	gs_render_start(true);
	for (int i = 0; i <= 360; i += (360 / 20)) {
		float pos = RAD(float(i));
		gs_vertex2f(cosf(pos), sinf(pos));
	}
	circle = gs_render_save();

	InitSafeAreas(&actionSafeMargin, &graphicsSafeMargin, &fourByThreeSafeMargin, &leftLine, &topLine, &rightLine);
	obs_leave_graphics();
}

void OBSBasic::UpdatePreviewScalingMenu()
{
	bool fixedScaling = ui->preview->IsFixedScaling();
	float scalingAmount = ui->preview->GetScalingAmount();
	if (!fixedScaling) {
		ui->actionScaleWindow->setChecked(true);
		ui->actionScaleCanvas->setChecked(false);
		ui->actionScaleOutput->setChecked(false);
		return;
	}

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	ui->actionScaleWindow->setChecked(false);
	ui->actionScaleCanvas->setChecked(scalingAmount == 1.0f);
	ui->actionScaleOutput->setChecked(scalingAmount == float(ovi.output_width) / float(ovi.base_width));
}

void OBSBasic::DrawBackdrop(float cx, float cy)
{
	if (!box)
		return;

	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "DrawBackdrop");

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	vec4 colorVal;
	vec4_set(&colorVal, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &colorVal);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_scale3f(float(cx), float(cy), 1.0f);

	gs_load_vertexbuffer(box);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_load_vertexbuffer(nullptr);

	GS_DEBUG_MARKER_END();
}

void OBSBasic::RenderMain(void *data, uint32_t, uint32_t)
{
	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "RenderMain");

	OBSBasic *window = static_cast<OBSBasic *>(data);
	obs_video_info ovi;

	obs_get_video_info(&ovi);

	window->previewCX = int(window->previewScale * float(ovi.base_width));
	window->previewCY = int(window->previewScale * float(ovi.base_height));

	gs_viewport_push();
	gs_projection_push();

	obs_display_t *display = window->ui->preview->GetDisplay();
	uint32_t width, height;
	obs_display_size(display, &width, &height);
	float right = float(width) - window->previewX;
	float bottom = float(height) - window->previewY;

	gs_ortho(-window->previewX, right, -window->previewY, bottom, -100.0f, 100.0f);

	window->ui->preview->DrawOverflow();

	/* --------------------------------------- */

	gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height), -100.0f, 100.0f);
	gs_set_viewport(window->previewX, window->previewY, window->previewCX, window->previewCY);

	if (window->IsPreviewProgramMode()) {
		window->DrawBackdrop(float(ovi.base_width), float(ovi.base_height));

		OBSScene scene = window->GetCurrentScene();
		obs_source_t *source = obs_scene_get_source(scene);
		if (source)
			obs_source_video_render(source);
	} else {
		obs_render_main_texture_src_color_only();
	}
	gs_load_vertexbuffer(nullptr);

	/* --------------------------------------- */

	gs_ortho(-window->previewX, right, -window->previewY, bottom, -100.0f, 100.0f);
	gs_reset_viewport();

	uint32_t targetCX = window->previewCX;
	uint32_t targetCY = window->previewCY;

	if (window->drawSafeAreas) {
		RenderSafeAreas(window->actionSafeMargin, targetCX, targetCY);
		RenderSafeAreas(window->graphicsSafeMargin, targetCX, targetCY);
		RenderSafeAreas(window->fourByThreeSafeMargin, targetCX, targetCY);
		RenderSafeAreas(window->leftLine, targetCX, targetCY);
		RenderSafeAreas(window->topLine, targetCX, targetCY);
		RenderSafeAreas(window->rightLine, targetCX, targetCY);
	}

	window->ui->preview->DrawSceneEditing();

	if (window->drawSpacingHelpers)
		window->ui->preview->DrawSpacingHelpers();

	window->DrawQualityScoreLabel();

	/* --------------------------------------- */

	gs_projection_pop();
	gs_viewport_pop();

	GS_DEBUG_MARKER_END();
}

static obs_source_t *CreateQualityScoreLabel(float pixelRatio)
{
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease font = obs_data_create();

#if defined(_WIN32)
	obs_data_set_string(font, "face", "Arial");
#elif defined(__APPLE__)
	obs_data_set_string(font, "face", "Helvetica");
#else
	obs_data_set_string(font, "face", "Monospace");
#endif
	obs_data_set_int(font, "flags", 1); // Bold text
	obs_data_set_int(font, "size", 16 * pixelRatio);

	obs_data_set_obj(settings, "font", font);
	obs_data_set_bool(settings, "outline", true);

#ifdef _WIN32
	obs_data_set_int(settings, "outline_color", 0x000000);
	obs_data_set_int(settings, "outline_size", 3);
	const char *text_source_id = "text_gdiplus";
#else
	const char *text_source_id = "text_ft2_source";
#endif

	return obs_source_create_private(text_source_id, "Preview quality score label", settings);
}

/* Signal callback from the WHIP output's quality scorer thread - only
 * stores the value; all Qt/graphics work happens in
 * DrawQualityScoreLabel on the graphics thread. */
void OBSBasic::OnQualityScore(void *data, calldata_t *cd)
{
	OBSBasic *window = static_cast<OBSBasic *>(data);
	window->qualityScoreValue.store((float)calldata_float(cd, "score"));
	window->qualityScoreUpdateNs.store(os_gettime_ns());
}

void OBSBasic::DrawQualityScoreLabel()
{
	/* Hide once the scorer stops feeding us (stream stopped, scorer
	 * disabled, or it gave up on decode errors). */
	const uint64_t ts = qualityScoreUpdateNs.load();
	if (!ts || os_gettime_ns() - ts > 15000000000ULL)
		return;

	const float pixelRatio = GetDevicePixelRatio();

	if (!qualityScoreLabel)
		qualityScoreLabel = CreateQualityScoreLabel(pixelRatio);
	if (!qualityScoreLabel)
		return;

	const float score = qualityScoreValue.load();
	if (fabsf(score - qualityScoreShown) >= 0.05f) {
		QString text = qualityScoreFormat.arg((double)score, 0, 'f', 1);
		OBSDataAutoRelease settings = obs_source_get_settings(qualityScoreLabel);
		obs_data_set_string(settings, "text", QT_TO_UTF8(text));
		obs_source_update(qualityScoreLabel, settings);
		qualityScoreShown = score;
	}

	const uint32_t labelCX = obs_source_get_width(qualityScoreLabel);
	const uint32_t labelCY = obs_source_get_height(qualityScoreLabel);
	if (!labelCX || !labelCY)
		return;

	uint32_t width, height;
	obs_display_size(ui->preview->GetDisplay(), &width, &height);

	/* Anchor to the widget's top-right corner: that's the letterbox
	 * band (free space) whenever the video doesn't fill the widget,
	 * and just the frame's edge otherwise. The current ortho puts the
	 * video's top-left at (0,0) with the widget spanning
	 * [-previewX .. width-previewX] x [-previewY .. height-previewY]. */
	const float margin = 10.0f * pixelRatio;
	const float x = float(width) - float(previewX) - float(labelCX) - margin;
	const float y = -float(previewY) + margin;

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate3f(x, y, 0.0f);
	obs_source_video_render(qualityScoreLabel);
	gs_matrix_pop();
}

void OBSBasic::ResizePreview(uint32_t cx, uint32_t cy)
{
	QSize targetSize;
	bool isFixedScaling;
	obs_video_info ovi;

	/* resize preview panel to fix to the top section of the window */
	targetSize = GetPixelSize(ui->preview);

	isFixedScaling = ui->preview->IsFixedScaling();
	obs_get_video_info(&ovi);

	if (isFixedScaling) {
		previewScale = ui->preview->GetScalingAmount();

		ui->preview->ClampScrollingOffsets();

		GetCenterPosFromFixedScale(int(cx), int(cy), targetSize.width() - PREVIEW_EDGE_SIZE * 2,
					   targetSize.height() - PREVIEW_EDGE_SIZE * 2, previewX, previewY,
					   previewScale);
		previewX += ui->preview->GetScrollX();
		previewY += ui->preview->GetScrollY();

	} else {
		GetScaleAndCenterPos(int(cx), int(cy), targetSize.width() - PREVIEW_EDGE_SIZE * 2,
				     targetSize.height() - PREVIEW_EDGE_SIZE * 2, previewX, previewY, previewScale);
	}

	ui->preview->SetScalingAmount(previewScale);

	previewX += float(PREVIEW_EDGE_SIZE);
	previewY += float(PREVIEW_EDGE_SIZE);
}

void OBSBasic::on_preview_customContextMenuRequested()
{
	CreateSourcePopupMenu(GetTopSelectedSourceItem(), true);
}

void OBSBasic::on_previewDisabledWidget_customContextMenuRequested()
{
	QMenu popup(this);
	delete previewProjectorMain;

	QAction *action = popup.addAction(QTStr("Basic.Main.PreviewConextMenu.Enable"), this, &OBSBasic::TogglePreview);
	action->setCheckable(true);
	action->setChecked(obs_display_enabled(ui->preview->GetDisplay()));

	previewProjectorMain = new QMenu(QTStr("Projector.Open.Preview"));
	AddProjectorMenuMonitors(previewProjectorMain, this, &OBSBasic::OpenPreviewProjector);
	previewProjectorMain->addSeparator();
	previewProjectorMain->addAction(QTStr("Projector.Window"), this, &OBSBasic::OpenPreviewWindow);

	popup.addMenu(previewProjectorMain);
	popup.exec(QCursor::pos());
}

void OBSBasic::EnablePreviewDisplay(bool enable)
{
	obs_display_set_enabled(ui->preview->GetDisplay(), enable);
	ui->previewContainer->setVisible(enable);
	ui->previewDisabledWidget->setVisible(!enable);
}

void OBSBasic::TogglePreview()
{
	previewEnabled = !previewEnabled;
	EnablePreviewDisplay(previewEnabled);
}

void OBSBasic::EnablePreview()
{
	if (previewProgramMode)
		return;

	previewEnabled = true;
	EnablePreviewDisplay(true);
}

void OBSBasic::DisablePreview()
{
	if (previewProgramMode)
		return;

	previewEnabled = false;
	EnablePreviewDisplay(false);
}

static bool nudge_callback(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_locked(item))
		return true;

	struct vec2 &offset = *static_cast<struct vec2 *>(param);
	struct vec2 pos;

	if (!obs_sceneitem_selected(item)) {
		if (obs_sceneitem_is_group(item)) {
			struct vec3 offset3;
			vec3_set(&offset3, offset.x, offset.y, 0.0f);

			struct matrix4 matrix;
			obs_sceneitem_get_draw_transform(item, &matrix);
			vec4_set(&matrix.t, 0.0f, 0.0f, 0.0f, 1.0f);
			matrix4_inv(&matrix, &matrix);
			vec3_transform(&offset3, &offset3, &matrix);

			struct vec2 new_offset;
			vec2_set(&new_offset, offset3.x, offset3.y);
			obs_sceneitem_group_enum_items(item, nudge_callback, &new_offset);
		}

		return true;
	}

	obs_sceneitem_get_pos(item, &pos);
	vec2_add(&pos, &pos, &offset);
	obs_sceneitem_set_pos(item, &pos);
	return true;
}

void OBSBasic::Nudge(int dist, MoveDir dir)
{
	if (ui->preview->Locked())
		return;

	struct vec2 offset;
	vec2_set(&offset, 0.0f, 0.0f);

	switch (dir) {
	case MoveDir::Up:
		offset.y = (float)-dist;
		break;
	case MoveDir::Down:
		offset.y = (float)dist;
		break;
	case MoveDir::Left:
		offset.x = (float)-dist;
		break;
	case MoveDir::Right:
		offset.x = (float)dist;
		break;
	}

	if (!recent_nudge) {
		recent_nudge = true;
		OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), true);
		std::string undo_data(obs_data_get_json(wrapper));

		nudge_timer = new QTimer;
		QObject::connect(nudge_timer, &QTimer::timeout, this, [this, &recent_nudge = recent_nudge, undo_data]() {
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), true);
			std::string redo_data(obs_data_get_json(rwrapper));

			undo_s.add_action(QTStr("Undo.Transform").arg(obs_source_get_name(GetCurrentSceneSource())),
					  undo_redo, undo_redo, undo_data, redo_data);

			recent_nudge = false;
		});
		connect(nudge_timer, &QTimer::timeout, nudge_timer, &QTimer::deleteLater);
		nudge_timer->setSingleShot(true);
	}

	if (nudge_timer) {
		nudge_timer->stop();
		nudge_timer->start(1000);
	} else {
		blog(LOG_ERROR, "No nudge timer!");
	}

	obs_scene_enum_items(GetCurrentScene(), nudge_callback, &offset);
}

void OBSBasic::on_actionLockPreview_triggered()
{
	ui->preview->ToggleLocked();
	ui->actionLockPreview->setChecked(ui->preview->Locked());
}

void OBSBasic::on_scalingMenu_aboutToShow()
{
	obs_video_info ovi;
	obs_get_video_info(&ovi);

	QAction *action = ui->actionScaleCanvas;
	QString text = QTStr("Basic.MainMenu.Edit.Scale.Canvas");
	text = text.arg(QString::number(ovi.base_width), QString::number(ovi.base_height));
	action->setText(text);

	action = ui->actionScaleOutput;
	text = QTStr("Basic.MainMenu.Edit.Scale.Output");
	text = text.arg(QString::number(ovi.output_width), QString::number(ovi.output_height));
	action->setText(text);
	action->setVisible(!(ovi.output_width == ovi.base_width && ovi.output_height == ovi.base_height));

	UpdatePreviewScalingMenu();
}

void OBSBasic::setPreviewScalingWindow()
{
	ui->preview->SetFixedScaling(false);
	ui->preview->ResetScrollingOffset();

	emit ui->preview->DisplayResized();
}

void OBSBasic::setPreviewScalingCanvas()
{
	ui->preview->SetFixedScaling(true);
	ui->preview->SetScalingLevel(0);

	emit ui->preview->DisplayResized();
}

void OBSBasic::setPreviewScalingOutput()
{
	obs_video_info ovi;
	obs_get_video_info(&ovi);

	ui->preview->SetFixedScaling(true);
	float scalingAmount = float(ovi.output_width) / float(ovi.base_width);
	// log base ZOOM_SENSITIVITY of x = log(x) / log(ZOOM_SENSITIVITY)
	int32_t approxScalingLevel = int32_t(round(log(scalingAmount) / log(ZOOM_SENSITIVITY)));
	ui->preview->SetScalingLevelAndAmount(approxScalingLevel, scalingAmount);
	emit ui->preview->DisplayResized();
}

static void ConfirmColor(SourceTree *sources, const QColor &color, QModelIndexList selectedItems)
{
	for (int x = 0; x < selectedItems.count(); x++) {
		SourceTreeItem *treeItem = sources->GetItemWidget(selectedItems[x].row());
		treeItem->setStyleSheet("background: " + color.name(QColor::HexArgb));
		treeItem->style()->unpolish(treeItem);
		treeItem->style()->polish(treeItem);

		OBSSceneItem sceneItem = sources->Get(selectedItems[x].row());
		OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
		obs_data_set_int(privData, "color-preset", 1);
		obs_data_set_string(privData, "color", QT_TO_UTF8(color.name(QColor::HexArgb)));
	}
}

void OBSBasic::ColorChange()
{
	QModelIndexList selectedItems = ui->sources->selectionModel()->selectedIndexes();
	QAction *action = qobject_cast<QAction *>(sender());
	QPushButton *colorButton = qobject_cast<QPushButton *>(sender());

	if (selectedItems.count() == 0)
		return;

	if (colorButton) {
		int preset = colorButton->property("bgColor").value<int>();

		for (int x = 0; x < selectedItems.count(); x++) {
			SourceTreeItem *treeItem = ui->sources->GetItemWidget(selectedItems[x].row());
			treeItem->setStyleSheet("");
			treeItem->setProperty("bgColor", preset);
			treeItem->style()->unpolish(treeItem);
			treeItem->style()->polish(treeItem);

			OBSSceneItem sceneItem = ui->sources->Get(selectedItems[x].row());
			OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
			obs_data_set_int(privData, "color-preset", preset + 1);
			obs_data_set_string(privData, "color", "");
		}

		for (int i = 1; i < 9; i++) {
			stringstream button;
			button << "preset" << i;
			QPushButton *cButton =
				colorButton->parentWidget()->findChild<QPushButton *>(button.str().c_str());
			cButton->setStyleSheet("border: 1px solid black");
		}

		colorButton->setStyleSheet("border: 2px solid black");
	} else if (action) {
		int preset = action->property("bgColor").value<int>();

		if (preset == 1) {
			OBSSceneItem curSceneItem = GetCurrentSceneItem();
			SourceTreeItem *curTreeItem = GetItemWidgetFromSceneItem(curSceneItem);
			OBSDataAutoRelease curPrivData = obs_sceneitem_get_private_settings(curSceneItem);

			int oldPreset = obs_data_get_int(curPrivData, "color-preset");
			const QString oldSheet = curTreeItem->styleSheet();

			auto liveChangeColor = [=](const QColor &color) {
				if (color.isValid()) {
					curTreeItem->setStyleSheet("background: " + color.name(QColor::HexArgb));
				}
			};

			auto changedColor = [this, selectedItems](const QColor &color) {
				if (color.isValid()) {
					ConfirmColor(ui->sources, color, selectedItems);
				}
			};

			auto rejected = [=]() {
				if (oldPreset == 1) {
					curTreeItem->setStyleSheet(oldSheet);
					curTreeItem->setProperty("bgColor", 0);
				} else if (oldPreset == 0) {
					curTreeItem->setStyleSheet("background: none");
					curTreeItem->setProperty("bgColor", 0);
				} else {
					curTreeItem->setStyleSheet("");
					curTreeItem->setProperty("bgColor", oldPreset - 1);
				}

				curTreeItem->style()->unpolish(curTreeItem);
				curTreeItem->style()->polish(curTreeItem);
			};

			QColorDialog::ColorDialogOptions options = QColorDialog::ShowAlphaChannel;

			const char *oldColor = obs_data_get_string(curPrivData, "color");
			const char *customColor = *oldColor != 0 ? oldColor : "#55FF0000";
#ifdef __linux__
			// TODO: Revisit hang on Ubuntu with native dialog
			options |= QColorDialog::DontUseNativeDialog;
#endif

			QColorDialog *colorDialog = new QColorDialog(this);
			colorDialog->setOptions(options);
			colorDialog->setCurrentColor(QColor(customColor));
			connect(colorDialog, &QColorDialog::currentColorChanged, this, liveChangeColor);
			connect(colorDialog, &QColorDialog::colorSelected, this, changedColor);
			connect(colorDialog, &QColorDialog::rejected, this, rejected);
			colorDialog->open();
		} else {
			for (int x = 0; x < selectedItems.count(); x++) {
				SourceTreeItem *treeItem = ui->sources->GetItemWidget(selectedItems[x].row());
				treeItem->setStyleSheet("background: none");
				treeItem->setProperty("bgColor", preset);
				treeItem->style()->unpolish(treeItem);
				treeItem->style()->polish(treeItem);

				OBSSceneItem sceneItem = ui->sources->Get(selectedItems[x].row());
				OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
				obs_data_set_int(privData, "color-preset", preset);
				obs_data_set_string(privData, "color", "");
			}
		}
	}
}

void OBSBasic::UpdateProjectorHideCursor()
{
	for (size_t i = 0; i < projectors.size(); i++)
		projectors[i]->SetHideCursor();
}

void OBSBasic::UpdateProjectorAlwaysOnTop(bool top)
{
	for (size_t i = 0; i < projectors.size(); i++)
		SetAlwaysOnTop(projectors[i], top);
}

void OBSBasic::ResetProjectors()
{
	OBSDataArrayAutoRelease savedProjectorList = SaveProjectors();
	ClearProjectors();
	LoadSavedProjectors(savedProjectorList);
}

void OBSBasic::UpdatePreviewSafeAreas()
{
	drawSafeAreas = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowSafeAreas");
}

void OBSBasic::UpdatePreviewOverflowSettings()
{
	bool hidden = config_get_bool(App()->GetUserConfig(), "BasicWindow", "OverflowHidden");
	bool select = config_get_bool(App()->GetUserConfig(), "BasicWindow", "OverflowSelectionHidden");
	bool always = config_get_bool(App()->GetUserConfig(), "BasicWindow", "OverflowAlwaysVisible");

	ui->preview->SetOverflowHidden(hidden);
	ui->preview->SetOverflowSelectionHidden(select);
	ui->preview->SetOverflowAlwaysVisible(always);
}

static inline QColor color_from_int(long long val)
{
	return QColor(val & 0xff, (val >> 8) & 0xff, (val >> 16) & 0xff, (val >> 24) & 0xff);
}

QColor OBSBasic::GetSelectionColor() const
{
	if (config_get_bool(App()->GetUserConfig(), "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(App()->GetUserConfig(), "Accessibility", "SelectRed"));
	} else {
		return QColor::fromRgb(255, 0, 0);
	}
}

QColor OBSBasic::GetCropColor() const
{
	if (config_get_bool(App()->GetUserConfig(), "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(App()->GetUserConfig(), "Accessibility", "SelectGreen"));
	} else {
		return QColor::fromRgb(0, 255, 0);
	}
}

QColor OBSBasic::GetHoverColor() const
{
	if (config_get_bool(App()->GetUserConfig(), "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(App()->GetUserConfig(), "Accessibility", "SelectBlue"));
	} else {
		return QColor::fromRgb(0, 127, 255);
	}
}

void OBSBasic::UpdatePreviewSpacingHelpers()
{
	drawSpacingHelpers = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SpacingHelpersEnabled");
}

float OBSBasic::GetDevicePixelRatio()
{
	return dpi;
}

void OBSBasic::UpdatePreviewControls()
{
	const int scalingLevel = ui->preview->GetScalingLevel();

	if (!ui->preview->IsFixedScaling()) {
		ui->previewXScrollBar->setRange(0, 0);
		ui->previewYScrollBar->setRange(0, 0);

		ui->actionPreviewResetZoom->setEnabled(false);

		return;
	}

	const bool minZoom = scalingLevel == MAX_SCALING_LEVEL;
	const bool maxZoom = scalingLevel == -MAX_SCALING_LEVEL;

	ui->actionPreviewZoomIn->setEnabled(!minZoom);
	ui->previewZoomInButton->setEnabled(!minZoom);

	ui->actionPreviewZoomOut->setEnabled(!maxZoom);
	ui->previewZoomOutButton->setEnabled(!maxZoom);

	ui->actionPreviewResetZoom->setEnabled(scalingLevel != 0);
}

void OBSBasic::UpdateRoiSelectButton()
{
	obs_service_t *svc = GetService();
	const bool isWhip = svc && strcmp(obs_service_get_type(svc), "whip_custom") == 0;
	const bool streaming = outputHandler && outputHandler->StreamingActive();

	ui->previewRoiSelectButton->setVisible(isWhip);
	ui->previewRoiSelectButton->setEnabled(isWhip && !streaming);

	// Disarm rather than leave a dangling armed tool if the button that
	// armed it just got hidden/disabled out from under the user (service
	// switched away from WHIP, or a stream just started).
	if ((!isWhip || streaming) && ui->preview->RoiSelectMode())
		ui->preview->SetRoiSelectMode(false);

	// Passive preview overlay showing the currently-saved region (see
	// OBSBasicPreview::SetRoiOverlay()) - kept up regardless of streaming
	// state (unlike the button above) since it's just a "here's what's
	// configured" indicator, still useful to check mid-stream if the
	// camera framing shifted.
	bool roiEnabled = false;
	int roiLeft = 0, roiTop = 0, roiRight = 0, roiBottom = 0;
	if (isWhip) {
		OBSDataAutoRelease settings = obs_service_get_settings(svc);
		roiEnabled = obs_data_get_bool(settings, "roi_enabled");
		roiLeft = (int)obs_data_get_int(settings, "roi_left");
		roiTop = (int)obs_data_get_int(settings, "roi_top");
		roiRight = (int)obs_data_get_int(settings, "roi_right");
		roiBottom = (int)obs_data_get_int(settings, "roi_bottom");
	}

	obs_video_info ovi;
	const bool haveVideo = obs_get_video_info(&ovi) && ovi.base_width && ovi.base_height;
	const uint32_t outputWidth = haveVideo ? video_output_get_width(obs_get_video()) : 0;
	const uint32_t outputHeight = haveVideo ? video_output_get_height(obs_get_video()) : 0;

	if (roiEnabled && haveVideo && outputWidth && outputHeight && roiRight > roiLeft && roiBottom > roiTop) {
		// Inverse of the canvas -> output scaling in OnRoiRegionSelected().
		const float scaleX = (float)ovi.base_width / (float)outputWidth;
		const float scaleY = (float)ovi.base_height / (float)outputHeight;

		ui->preview->SetRoiOverlay(true, roiLeft * scaleX, roiTop * scaleY, roiRight * scaleX,
					   roiBottom * scaleY);
	} else {
		ui->preview->SetRoiOverlay(false);
	}
}

void OBSBasic::OnRoiRegionSelected(float left, float top, float right, float bottom)
{
	obs_service_t *svc = GetService();
	if (!svc || strcmp(obs_service_get_type(svc), "whip_custom") != 0)
		return;

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
		return;

	const uint32_t outputWidth = video_output_get_width(obs_get_video());
	const uint32_t outputHeight = video_output_get_height(obs_get_video());
	if (!outputWidth || !outputHeight)
		return;

	// roi_left/top/right/bottom are defined at the WHIP output's actual
	// (scaled) resolution - see WHIPOutput::ApplyRoi() - which generally
	// differs from the canvas/base resolution the drag was drawn in.
	const float scaleX = (float)outputWidth / (float)ovi.base_width;
	const float scaleY = (float)outputHeight / (float)ovi.base_height;

	const int roiLeft = (int)std::clamp(std::round(std::min(left, right) * scaleX), 0.0f, (float)outputWidth);
	const int roiTop = (int)std::clamp(std::round(std::min(top, bottom) * scaleY), 0.0f, (float)outputHeight);
	const int roiRight = (int)std::clamp(std::round(std::max(left, right) * scaleX), 0.0f, (float)outputWidth);
	const int roiBottom = (int)std::clamp(std::round(std::max(top, bottom) * scaleY), 0.0f, (float)outputHeight);

	// An accidental click (no real drag) yields a near-zero rect - ignore
	// it instead of saving a degenerate ROI.
	if (roiRight - roiLeft < 8 || roiBottom - roiTop < 8)
		return;

	OBSDataAutoRelease settings = obs_service_get_settings(svc);
	obs_data_set_bool(settings, "roi_enabled", true);
	obs_data_set_int(settings, "roi_left", roiLeft);
	obs_data_set_int(settings, "roi_top", roiTop);
	obs_data_set_int(settings, "roi_right", roiRight);
	obs_data_set_int(settings, "roi_bottom", roiBottom);

	SaveService();

	// Refreshes the passive overlay to the rect just saved above, instead
	// of leaving it showing whatever was there before this drag.
	UpdateRoiSelectButton();

	ui->statusbar->showMessage(QTStr("Basic.Main.Preview.SetRoi.Saved")
					    .arg(roiLeft)
					    .arg(roiTop)
					    .arg(roiRight)
					    .arg(roiBottom),
				    5000);
}

void OBSBasic::PreviewScalingModeChanged(int value)
{
	switch (value) {
	case 0:
		setPreviewScalingWindow();
		break;
	case 1:
		setPreviewScalingCanvas();
		break;
	case 2:
		setPreviewScalingOutput();
		break;
	};
}
