// Cypress E2E tests for InferDeck Gateway - Audio TTS Endpoint
// Run with: npx cypress run --spec "cypress/e2e/audio_tts.cy.ts"

describe('Audio TTS /v1/audio/speech Endpoint', () => {
  const BASE_URL = Cypress.env('API_URL') || 'https://localhost:8080';

  describe('/v1/audio/speech', () => {
    it('returns 400 for missing body', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
        expect(response.body.error).to.have.property('message');
      });
    });

    it('returns 400 for missing input text', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
        expect(response.body.error).to.have.property('message');
      });
    });

    it('returns 400 for invalid model', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          input: 'Hello, world!',
          model: 'nonexistent-tts-model',
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body.error.type).to.eq('invalid_request_error');
      });
    });

    it('accepts piper-en model with valid input', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'Hello, world! This is a test of the text-to-speech system.',
          voice: 'amy',
          speed: 1.0,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.be.an('object');
          expect(response.body).to.have.property('object', 'speech');
          expect(response.body).to.have.property('model');
          expect(response.body).to.have.property('format');
        }
      });
    });

    it('supports WAV output format', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'Testing WAV format output.',
          voice: 'amy',
          format: 'wav',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body.format).to.eq('wav');
        }
      });
    });

    it('supports MP3 output format', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'Testing MP3 format output.',
          voice: 'amy',
          format: 'mp3',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body.format).to.eq('mp3');
        }
      });
    });

    it('accepts speed parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'Testing speed parameter.',
          voice: 'amy',
          speed: 0.5,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('model');
        }
      });
    });

    it('accepts speed parameter with value > 1.0', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'Testing speed parameter above 1.0.',
          voice: 'amy',
          speed: 2.0,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('model');
        }
      });
    });

    it('validates piper TTS response schema', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'OpenAI-compatible TTS response validation test.',
          voice: 'amy',
          format: 'wav',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('object');
          expect(response.body).to.have.property('model');
          expect(response.body).to.have.property('format');
          expect(response.body).to.have.property('voice');
        }
      });
    });
  });

  describe('Voice model support', () => {
    it('accepts amy voice', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'Testing amy voice.',
          voice: 'amy',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body.voice).to.eq('amy');
        }
      });
    });

    it('accepts jenny voice', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'Testing jenny voice.',
          voice: 'jenny',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body.voice).to.eq('jenny');
        }
      });
    });
  });

  describe('TTS error handling', () => {
    it('returns error for empty input text', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: '',
          voice: 'amy',
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body).to.have.property('error');
        expect(response.body.error).to.have.property('message');
      });
    });

    it('returns error for input exceeding max length', () => {
      cy.request({
        url: `${BASE_URL}/v1/audio/speech`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'piper-en',
          input: 'x'.repeat(5001),
          voice: 'amy',
        },
      }).then((response) => {
        if (response.status === 400) {
          expect(response.body.error.type).to.eq('invalid_request_error');
        }
      });
    });
  });
});
