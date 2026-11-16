## Multistream OBS 1.2.0

- Extensão Flatpak para OBS Studio 33 em Linux x86-64.
- Escolha da região de ingestão da Twitch, com lista local e atualização pela lista oficial.
- Conexão de conta para importar os dados de transmissão da Twitch e da Kick. A Kick exige as credenciais de um aplicativo de desenvolvedor próprio.
- Lista editável de resoluções calculadas como no OBS.
- Correção para URLs de ingestão da Kick sem `/app` e para o intervalo automático de quadros-chave em destinos Kick existentes.

As correções da Kick e o processo de autorização ainda dependem de validação com contas reais. Tokens OAuth e o segredo do aplicativo Kick não são salvos pelo plugin; a chave importada é salva na configuração do perfil do OBS.
