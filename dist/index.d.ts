/// <reference types="node" />
/// <reference types="node" />
import { Window } from "./classes/window";
import { EventEmitter } from "events";
import { Monitor } from "./classes/monitor";
import { EmptyMonitor } from "./classes/empty-monitor";
import { IWindowSummary } from "./interfaces";
declare const addon: any;
declare class WindowManager extends EventEmitter {
    constructor();
    requestAccessibility: () => any;
    requestScreenCapture: () => any;
    getActiveWindow: () => Window;
    getWindows: () => Window[];
    getMonitors: () => Monitor[];
    getPrimaryMonitor: () => Monitor | EmptyMonitor;
    createProcess: (path: string, cmd?: string) => number;
    hideInstantly: (handle: Buffer) => any;
    forceWindowPaint: (handle: Buffer) => any;
    setWindowAsPopup: (handle: Buffer) => any;
    setWindowAsPopupWithRoundedCorners: (handle: Buffer) => any;
    showInstantly: (handle: Buffer) => any;
    getWindowsSummary: () => IWindowSummary[];
}
declare const windowManager: WindowManager;
export { windowManager, Window, addon };
