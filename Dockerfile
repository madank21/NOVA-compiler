FROM node:22-alpine

WORKDIR /app

RUN apk add --no-cache build-base

COPY package*.json ./

RUN npm ci

COPY . .

EXPOSE 3000

CMD ["npm", "run", "dev", "--", "--host", "0.0.0.0"]