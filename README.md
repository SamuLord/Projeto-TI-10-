# GridGuard QEE — ESP32 + MQTT + supervisório web

## Arquitetura
- `esp32_qee.ino`: firmware Arduino para relés, lógica de retaguarda e amostragem analógica.
- `backend.py`: ponte MQTT, API HTTP e persistência em SQLite.
- `web/index.html`: interface web com diagrama unifilar, controles, gráfico de tensão e relatórios.
- Broker MQTT: Mosquitto (gratuito).

## Preparação
1. Instale o Arduino IDE e o suporte a placas ESP32.
2. Instale as bibliotecas `PubSubClient` e `ArduinoJson`.
3. Edite `WIFI_SSID`, `WIFI_PASS` e `MQTT_HOST` no sketch. `MQTT_HOST` deve ser o IP do computador que executa Mosquitto.
4. Instale o Mosquitto no computador e configure-o para aceitar conexões da rede local na porta 1883. Para uma rede real, use autenticação e TLS.
5. No computador, instale Python 3.10+ e execute:
   ```bash
   pip install flask paho-mqtt
   python backend.py
   ```
6. Abra `http://localhost:5000` no navegador.
7. Carregue `esp32_qee.ino` no ESP32.

## Tópicos MQTT
- `qee/esp32-qee-01/cmd`: comandos publicados pelo backend.
- `qee/esp32-qee-01/status`: estado confirmado dos relés.
- `qee/esp32-qee-01/events`: eventos de atuação e religamento.
- `qee/esp32-qee-01/wave`: lote de amostras para o gráfico.

## Pinos padrão (ESP32 clássico)
- R1: GPIO 25
- R2: GPIO 26
- R3: GPIO 27
- Sinal condicionado de tensão: GPIO 34 (ADC1)

Confirme o pinout da placa e o tipo de acionamento dos módulos. Muitos módulos de relé são ativos em LOW; outros não.

## Segurança e limitações
- **Nunca conecte tensão CA da rede elétrica diretamente ao ESP32.** Use fonte isolada de baixa tensão, divisor/condicionador com proteção apropriada e, para medições isoladas, um sensor adequado. A entrada ADC aceita apenas faixa positiva dentro dos limites do chip; uma senoide bipolar precisa de offset e condicionamento.
- Os parâmetros `ADC_COUNTS_PER_VOLT_INPUT`, `ADC_OFFSET_COUNTS` e `VOLTAGE_SCALE` são exemplos e precisam ser calibrados. O sketch não é instrumento de medição certificado.
- O código de onda usa amostragem didática de aproximadamente 1 kHz. Para análise metrológica/qualidade de energia, aumente a taxa, use aquisição adequada, calcule RMS por janelas e valide o condicionamento.
- A lógica assume `true/closed` como relé fechado. Confirme se o seu hardware está usando relé como contato normalmente aberto e ajuste a lógica de acordo com o circuito real.
- Para falha de atuação simulada, o evento contabilizado como trip é R1, como solicitado. O backend mantém os eventos de R1/R2/R3.
- O backend grava os dados no arquivo `qee_history.sqlite3` no diretório de execução.
- DEC/FEC aqui são indicadores demonstrativos por ramal. A apuração regulatória requer definição do conjunto, quantidade de UCs e critérios oficiais de interrupção.
- O cálculo real de DRP/DRC não está habilitado no firmware inicial: requer tensão RMS calibrada, tensão nominal de referência e janela/quantidade de medições conforme PRODIST. Não classifique a tensão apenas pela amplitude instantânea.

## Próximos passos recomendados
1. Confirmar modelo exato do ESP32, módulo de relé e circuito de condicionamento.
2. Calibrar a medição contra multímetro confiável.
3. Implementar buffer circular de amostras e cálculo RMS por ciclo.
4. Adicionar autenticação MQTT e tratamento de comandos duplicados.
5. Testar cada cenário com LEDs/baixa tensão antes de qualquer carga.
