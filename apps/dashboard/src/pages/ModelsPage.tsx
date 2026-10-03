import React from 'react';
import { type DashboardSection } from '../dashboardSections';
import { ModelStoreHubPanel } from './ModelStoreHubPanel';

export const ModelsPage: React.FC<{ section?: DashboardSection; repo?: string }> = ({ section = 'llm', repo }) => (
  <ModelStoreHubPanel section={section} repo={repo} />
);
