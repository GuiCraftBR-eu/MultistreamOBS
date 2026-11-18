## Multistream OBS 1.2.2

- Recupera destinos do backup do perfil quando o arquivo principal de configurações está danificado.
- Preserva os arquivos e desativa o salvamento automático quando nem as configurações nem o backup podem ser lidos. O painel mostra um aviso e permite restaurar uma exportação válida.
- Rejeita arquivos de importação inválidos ou incompatíveis, informa falhas de gravação e mantém os destinos atuais se a importação não puder ser salva. A importação também é bloqueada enquanto um destino está conectando.
- Salva exportações e configurações de forma atômica, com verificações para evitar arquivos incompletos e proteger o backup válido.

As configurações e exportações ainda contêm chaves de transmissão em texto simples. Mantenha esses arquivos privados.
