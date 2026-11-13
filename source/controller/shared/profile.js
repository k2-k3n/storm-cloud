// Storm Cloud: what the controllers show and the CC each control sends (channel 1).
// src/app.cpp binds the same numbers in kBind.
export const profile = {
  name: 'Storm Cloud',
  accent: '#5b7fd6',
  pages: [
    {key: 'P', label: 'PLAY', title: 'Make weather', color: '#7FA6E8', controls: [
      {id: 'downpour', cc: 30, kind: 'trigger', label: 'POUR', key: 'A'},
      {id: 'gale', cc: 31, kind: 'trigger', label: 'GALE', key: 'S'},
      {id: 'storm', cc: 32, kind: 'trigger', label: 'STORM', key: 'D'},
      {id: 'sun', cc: 33, kind: 'trigger', label: 'SUN', key: 'F'},
      {id: 'sky', kind: 'xy', label: 'Rain × Wind', x: 'rain', y: 'wind'},
    ]},
    {key: 'W', label: 'WEATHER', title: 'The weather', color: '#9AB8C9', controls: [
      {id: 'rain', cc: 20, kind: 'range', label: 'Rain', min: 0, max: 100, unit: '%', def: 50},
      {id: 'wind', cc: 21, kind: 'range', label: 'Wind', min: 0, max: 100, unit: '%', def: 40},
      {id: 'thunder', cc: 22, kind: 'toggle', label: 'Thunder', def: 1},
      {id: 'thunderAmt', cc: 23, kind: 'range', label: 'Thunder level', min: 0, max: 100, unit: '%', def: 60},
    ]},
    {key: 'S', label: 'SKY', title: 'The valley', color: '#F2C94C', controls: [
      {id: 'space', cc: 24, kind: 'choice', label: 'Space', options: ['SMALL', 'WIDE', 'HUGE'], def: 1},
      {id: 'mix', cc: 25, kind: 'range', label: 'Mix', min: 0, max: 100, unit: '%', def: 100},
      {id: 'level', cc: 26, kind: 'range', label: 'Level', min: -12, max: 6, unit: 'dB', def: 0},
    ]},
  ],
};
