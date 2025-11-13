export interface IRectangle {
  x?: number;
  y?: number;
  width?: number;
  height?: number;
}

export interface IMonitorInfo {
  id: number;
  bounds?: IRectangle;
  isPrimary?: boolean;
  workArea?: IRectangle;
}

export interface IWindowSummary {
  id: number;
  title: string;
  path: string;
  processId: number;
  bounds: IRectangle;
  zOrder: number;
  isVisible: boolean;
}
