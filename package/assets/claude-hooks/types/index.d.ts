// Native session values of this plugin. Claude Code checks the hooks module's
// $.state calls against this declaration.
declare module 'claude-code' {
    interface PluginState {
        'logonomic-claude-hooks': {
            // Written once by each module copy as it loads; only the version of
            // that write is used, to order the copies.
            module_load: true;
            model_sequence: {
                // Load version of the module copy that owns model sequencing.
                owner: number;
                // Highest decimal model sequence published.
                sequence: string;
            };
        };
    }
}
