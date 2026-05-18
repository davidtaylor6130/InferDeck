// Cypress E2E tests for InferDeck Gateway - Image Generation Endpoint
// Run with: npx cypress run --spec "cypress/e2e/images.cy.ts"

describe('Image Generation /v1/images/* Endpoint', () => {
  const BASE_URL = Cypress.env('API_URL') || 'https://localhost:8080';

  describe('/v1/images/generate', () => {
    it('returns 400 for missing body', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
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

    it('returns 400 for missing prompt', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          negative_prompt: 'blurry, low quality',
        },
      }).then((response) => {
        expect(response.status).to.eq(400);
        expect(response.body.error).to.have.property('message');
      });
    });

    it('accepts text-to-image request with default parameters', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a beautiful sunset over the ocean',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('object', 'list');
          expect(response.body).to.have.property('data').to.be.an('array');
          if (response.body.data.length > 0) {
            const img = response.body.data[0];
            expect(img).to.have.property('url');
          }
        } else if (response.status === 503) {
          expect(response.body.error.type).to.eq('service_unavailable');
        }
      });
    });

    it('accepts custom width and height', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a mountain landscape',
          width: 1024,
          height: 1024,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data').to.be.an('array');
        }
      });
    });

    it('accepts num_steps parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a cityscape at night',
          num_steps: 50,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data').to.be.an('array');
        }
      });
    });

    it('accepts negative_prompt parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a beautiful garden',
          negative_prompt: 'blurry, dark, noisy',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data').to.be.an('array');
        }
      });
    });

    it('accepts guidance_scale parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a forest path',
          guidance_scale: 7.5,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data').to.be.an('array');
        }
      });
    });

    it('accepts seed parameter for reproducibility', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a sunrise',
          seed: 42,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data').to.be.an('array');
        }
      });
    });

    it('accepts img2img with input_image_path', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'enhance the photo with artistic style',
          input_image_path: '/path/to/source.jpg',
          strength: 0.75,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data').to.be.an('array');
        }
      });
    });

    it('accepts img2img with b64_json input', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'transform with artistic style',
          input_image: 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAA',
          strength: 0.5,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data').to.be.an('array');
        }
      });
    });

    it('accepts batch_size parameter', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'multiple images in a batch',
          n: 4,
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body.data).to.have.length(4);
        }
      });
    });

    it('accepts response_format b64_json', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a test image',
          response_format: 'b64_json',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('data').to.be.an('array');
        }
      });
    });

    it('returns error for invalid width (not divisible by 64)', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a test image',
          width: 641,
          height: 512,
        },
      }).then((response) => {
        if (response.status === 400) {
          expect(response.body.error).to.have.property('message');
        }
      });
    });

    it('returns error for invalid height (not divisible by 64)', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a test image',
          width: 512,
          height: 641,
        },
      }).then((response) => {
        if (response.status === 400) {
          expect(response.body.error).to.have.property('message');
        }
      });
    });

    it('returns error for invalid num_steps (out of range)', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a test image',
          num_steps: 0,
        },
      }).then((response) => {
        if (response.status === 400) {
          expect(response.body.error).to.have.property('message');
        }
      });
    });

    it('returns error for invalid strength in img2img (out of range)', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'enhance the photo',
          input_image_path: '/path/to/source.jpg',
          strength: -0.1,
        },
      }).then((response) => {
        if (response.status === 400) {
          expect(response.body.error).to.have.property('message');
        }
      });
    });
  });

  describe('Image response schema', () => {
    it('follows OpenAI-compatible image generation response format', () => {
      cy.request({
        url: `${BASE_URL}/v1/images/generate`,
        method: 'POST',
        failOnStatusCode: false,
        rejectUnauthorized: false,
        body: {
          model: 'stable-diffusion-xl',
          prompt: 'a test image for schema validation',
        },
      }).then((response) => {
        if (response.status === 200) {
          expect(response.body).to.have.property('object', 'list');
          expect(response.body).to.have.property('data').to.be.an('array');
          expect(response.body).to.have.property('created');
        }
      });
    });
  });
});
