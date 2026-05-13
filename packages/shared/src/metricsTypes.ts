export interface MetricsSample {
  source: string;
  metricName: string;
  metricValue: number;
  unit: string;
  createdAt: string;
}

export interface MetricsSummary {
  queueLengthHistory: number[];
  gpuUtilizationAvg: number | null;
  requestsLastHour: number;
  errorsLastHour: number;
  avgJobDuration: number | null;
  p50JobDuration: number | null;
  p95JobDuration: number | null;
  jobSuccessRate: number | null;
  gpuLockHoldTimeAvg: number | null;
}

export interface MetricsResponse {
  samples: MetricsSample[];
  summary: MetricsSummary;
}

export interface HardwareMetrics {
  gpu: {
    utilization: number | null;
    memoryUsed: number | null;
    memoryTotal: number | null;
    temperature: number | null;
    powerDraw: number | null;
    clockSpeed: number | null;
  };
  cpu: {
    utilization: number | null;
    loadAvg: [number, number, number];
  };
  memory: {
    used: number | null;
    total: number | null;
    percentage: number | null;
  };
  disk: {
    used: number | null;
    total: number | null;
    free: number | null;
    percentage: number | null;
  };
  timestamp: string;
}
