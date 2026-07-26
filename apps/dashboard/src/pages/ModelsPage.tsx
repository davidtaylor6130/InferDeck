import React from 'react';
import { type DashboardSection } from '../dashboardSections';
import { ModelStorePanel } from './ModelStorePanel';

export const ModelsPage: React.FC<{ section?: DashboardSection }> = ({ section = 'llm' }) => (
  <ModelStorePanel section={section} />
);
