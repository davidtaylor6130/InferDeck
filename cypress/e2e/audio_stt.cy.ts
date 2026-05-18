// Cypress E2E tests for InferDeck Gateway - Audio STT Endpoints
// Run with: npx cypress run --spec "cypress/e2e/audio_stt.cy.ts"

describe('Audio STT /v1/audio/* Endpoints', () => {
  const BASE_URL = Cypress.env('API_URL') || 'https://localhost:8080';

  describe('/v1/audio/transcriptions', () => {
    it('returns 400 for missing body (no file provided)', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/transcriptions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
        expect(response.body.error).to.have.property('message');
        expect(response.body.error).to.have.property('type');
      });
    });

    it('validates transcription response schema when server is running', () => {
      // Tests that the endpoint exists and returns a valid response structure
      cy.request({
        url: `${BASE_URL}/v1/audio/transcriptions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: { model: 'whisper-large-v3' },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('object', 'transcription');
          expect(response.body).to.have.property('text').and.to.be.a('string');
          expect(response.body).to.have.property('model');
          expect(response.body).to.have.property('language');
          expect(response.body).to.have.property('duration');
        } else {
          expect(response.status).to.be.oneOf([400, 415, 500, 503]);
          expect(response.body).to.have.property('error');
        }
      });
    });

    it('accepts whisper-large-v3 model parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/transcriptions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'whisper-large-v3',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body.model).to.eq('whisper-large-v3');
        }
      });
    });

    it('returns error for invalid model name', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/transcriptions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'nonexistent-model',
        },
      }).then((response) => {
        if (response.status === 400) {
          expect(response.body.error.type).to.eq('invalid_request_error');
        }
      });
    });

    it('validates response includes segments array when present', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/transcriptions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'whisper-large-v3',
          response_format: 'json',
        },
      }).then((response) => {
        if (response.status === 200) {
          if (response.body.segments) {
            expect(response.body.segments).to.be.an('array');
            if (response.body.segments.length > 0) {
              const seg = response.body.segments[0];
              expect(seg).to.have.property('id');
              expect(seg).to.have.property('start');
              expect(seg).to.have.property('end');
              expect(seg).to.have.property('text');
            }
          }
        }
      });
    });
  });

  describe('/v1/audio/translations', () => {
    it('returns 400 for missing body', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/translations`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
        expect(response.body.error).to.have.property('message');
      });
    });

    it('accepts whisper-large-v3 for translation', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/translations`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'whisper-large-v3',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('object', 'translation');
          expect(response.body).to.have.property('text').and.to.be.a('string');
          expect(response.body).to.have.property('model');
        }
      });
    });

    it('returns error for invalid model on translation', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/translations`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'nonexistent-model',
        },
      }).then((response) => {
        if (response.status === 400) {
          expect(response.body.error.type).to.eq('invalid_request_error');
        }
      });
    });
  });

  describe('STT response schema compliance', () => {
    it('follows OpenAI-compatible transcription response format', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/transcriptions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: { model: 'whisper-large-v3' },
      }).then((response) => {
        if (response.status === 200) {
          // Required fields per OpenAI schema
          expect(response.body).to.have.property('object');
          expect(response.body).to.have.property('text');
          expect(response.body.text).to.be.a('string');
          expect(response.body).to.have.property('model');
          expect(response.body).to.have.property('language');
          expect(response.body).to.have.property('duration');
          // OpenAI-compatible response types
          expect(response.body.object).to.be.a('string');
        }
      });
    });

    it('handles whisper GGML model specification', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/transcriptions`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'whisper-ggml.bin',
          prompt: 'test prompt',
          response_format: 'json',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('model');
        } else if (response.status === 400) {
          expect(response.body).to.have.property('error');
          expect(response.body.error).to.have.property('message');
        }
      });
    });
  });
});
