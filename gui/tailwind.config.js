/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{js,jsx}'],
  theme: {
    extend: {
      colors: {
        surface: {
          bg: '#0d1117',
          card: '#161b22',
          border: '#30363d',
          hover: '#1c2129',
        },
        profit: '#00c853',
        loss: '#ff1744',
        accent: '#58a6ff',
      },
    },
  },
  plugins: [],
};
