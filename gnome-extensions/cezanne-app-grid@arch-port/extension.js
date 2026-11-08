// Phone app grid, GNOME 51.
//
// The stock grid picks its page layout by aspect ratio among 8x3, 6x4, 4x6
// and 3x8 and then the largest icon of 96 / 64 / 48 / 32 / 24 / 16 that fits.
// In portrait on this 540x1200 (logical) screen that came out as 3 columns
// of small icons: 10 % of the width on each side is kept free for the
// neighbour page previews and arrows, and a workspace strip takes 15 % of
// the height above the grid.
//
// Here: phone layouts (6x4 in portrait, 3x8 / 2x8 in landscape, still
// picked by aspect ratio, so rotating works), the largest icon from a finer
// list of sizes that fits, no margins for page previews (pages are swiped)
// and no workspaces in the app grid.
//
// Shell internals touched: IconGridLayout.prototype._findBestIconSize,
// BaseAppViewGridLayout._getIndicatorsWidth (overridden),
// ControlsManagerLayout._computeWorkspacesBoxForState (wrapped on the
// instance), the app
// grid's setGridModes() and layout _pageWidth (forces a new fit), the
// overview controls' _stateAdjustment.
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {IconGridLayout} from 'resource:///org/gnome/shell/ui/iconGrid.js';
import {ControlsState} from 'resource:///org/gnome/shell/ui/overviewControls.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

const GRID_MODES = [
    {rows: 6, columns: 4},
    {rows: 3, columns: 8},
    {rows: 2, columns: 8},
];
const ICON_SIZES = [128, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16];
const NO_WORKSPACES_CLASS = 'cezanne-app-grid-no-workspaces';

export default class CezanneAppGrid extends Extension {
    enable() {
        const controls = Main.overview._overview._controls;
        this._controls = controls;
        this._appDisplay = controls._appDisplay;
        this._injections = new InjectionManager();

        // the stock logic (IconGridLayout._findBestIconSize) with more sizes
        this._injections.overrideMethod(IconGridLayout.prototype,
            '_findBestIconSize', () => function () {
                if (this.fixedIconSize !== -1)
                    return this.fixedIconSize;

                const nColumns = this.columnsPerPage;
                const nRows = this.rowsPerPage;
                const freeWidth = this._pageWidth -
                    this.columnSpacing * (nColumns - 1) -
                    this.pagePadding.left - this.pagePadding.right;
                const freeHeight = this._pageHeight -
                    this.rowSpacing * (nRows - 1) -
                    this.pagePadding.top - this.pagePadding.bottom;
                const [firstItem] = this._container;

                for (const size of ICON_SIZES) {
                    let itemSize = size;
                    if (firstItem) {
                        firstItem.icon.setIconSize(size);
                        const [width, height] = firstItem.get_preferred_size();
                        itemSize = Math.max(width, height);
                    }
                    if (itemSize * nColumns <= freeWidth && itemSize * nRows < freeHeight)
                        return size;
                }
                return ICON_SIZES.at(-1);
            });

        // pages are swiped: no side margins for the neighbour page previews
        this._injections.overrideMethod(
            Object.getPrototypeOf(this._appDisplay._appGridLayout),
            '_getIndicatorsWidth', () => () => 0);

        // no workspaces in the app grid: 1 px high at the screen top, behind
        // the panel (0 high makes the workspace layout scale by 0 -> NaN
        // allocations), the grid moves up under the search entry. On the instance:
        // dash-to-dock reinstalls its own prototype override on every monitor
        // change (rotation), which would drop one made there.
        const layout = controls.layout_manager;
        layout._computeWorkspacesBoxForState = function (state, ...args) {
            const proto = Object.getPrototypeOf(this);
            const box = proto._computeWorkspacesBoxForState.call(this, state, ...args);
            if (state === ControlsState.APP_GRID) {
                box.set_origin(box.x1, 0);
                box.set_size(box.get_width(), 1);
            }
            return box;
        };
        controls._stateAdjustment.connectObject('notify::value',
            () => this._syncWorkspaceShadow(), this);
        this._syncWorkspaceShadow();

        this._appDisplay._grid.setGridModes(GRID_MODES);
        this._refit();
    }

    disable() {
        this._controls._stateAdjustment.disconnectObject(this);
        Main.layoutManager.uiGroup.remove_style_class_name(NO_WORKSPACES_CLASS);
        this._injections.clear();
        this._injections = null;
        delete this._controls.layout_manager._computeWorkspacesBoxForState;
        this._appDisplay._grid.setGridModes(null);
        this._refit();
        this._controls = this._appDisplay = null;
    }

    // towards / in the app grid: the 1 px workspace box casts no shadow
    _syncWorkspaceShadow() {
        const inAppGrid = this._controls._stateAdjustment.value > ControlsState.WINDOW_PICKER;
        if (inAppGrid)
            Main.layoutManager.uiGroup.add_style_class_name(NO_WORKSPACES_CLASS);
        else
            Main.layoutManager.uiGroup.remove_style_class_name(NO_WORKSPACES_CLASS);
    }

    // icon sizes are only fitted again when the page size changes
    _refit() {
        const grid = this._appDisplay._grid;
        grid.layout_manager._pageWidth = 0;
        grid.queue_relayout();
        this._controls.layout_manager.layout_changed();
    }
}
