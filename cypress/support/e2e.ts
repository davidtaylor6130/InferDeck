// Cypress support file for InferDeck E2E tests

// Automatically retry failed requests (self-signed TLS)
Cypress.Overrides.applyDefaults();

beforeEach(() => {
  cy.intercept('GET', '**/v1/health', (req) => {
    req.headers['Accept'] = 'application/json';
  }).as('health');
});
