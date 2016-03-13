<?php
/**
 * @file
 * Quickpay and QuickpayTransaction classes.
 */

class QuickpayException extends Exception {}

/**
 * The Quickpay class abstracts a specific setup. Each QuickpayTransaction is
 * associated with a Quickpay class, containing the configuration.
 */
class Quickpay {
  protected static $currency_info = array();
  protected $test_mode = FALSE;
  protected $debug = FALSE;
  protected $merchant;
  protected $private_key;
  protected $agreement_id;
  protected $agreement_api_key;
  protected $order_prefix = '';
  protected $accepted_cards = array('creditcard');
  protected $popup = FALSE;
  // With a 'use user language' default option.
  protected $language = LANGUAGE_NONE;

  /**
   * Constructor. Takes a settings array.
   */
  public function __construct($options) {
    // Check options, throw exception if problems.
    if (empty($options['merchant']) || !preg_match('/^\d+$/', $options['merchant'])) {
      throw new QuickpayException(t('Invalid merchant number'));
    }
    $this->merchant = $options['merchant'];

    if (empty($options['private_key'])) {
      throw new QuickpayException(t('Empty private_key.'));
    }
    $this->private_key = $options['private_key'];

    if (empty($options['agreement_id'])) {
      throw new QuickpayException(t('Empty agreement_id.'));
    }
    $this->agreement_id = $options['agreement_id'];

    if (empty($options['agreement_api_key'])) {
      throw new QuickpayException(t('Empty agreement_api_key.'));
    }
    $this->agreement_api_key = $options['agreement_api_key'];

    if (isset($options['test_mode'])) {
      $this->test_mode = (bool) $options['test_mode'];
    }
    if (isset($options['debug'])) {
      $this->debug = (bool) $options['debug'];
    }
    if (isset($options['order_prefix'])) {
      $this->order_prefix = $options['order_prefix'];
    }
    // @todo validate this
    if (isset($options['accepted_cards'])) {
      $this->accepted_cards = $options['accepted_cards'];
    }
    if (isset($options['language'])) {
      $this->language = $options['language'];
    }
    /* if (isset($options['popup'])) { */
    /*   $this->popup = (bool) $options['popup']; */
    /* } */
  }

  /**
   * Get a transaction associated with this Quickpay instance.
   */
  public function transaction($txnid) {
    return new QuickpayTransaction($this, $txnid);
  }

  /**
   * Load a transaction from HTTP POST.
   *
   * @return
   *  Either a QuickpayTransaction object, or NULL if there was a problem.
   */
  public function transaction_callback() {
    // Copied from other module.
    // Can't use $_POST with JSON request.
    $request_body = file_get_contents("php://input");
    $request = json_decode($request_body);

    // Check that it validates.
    $checksum_calculated = $this->get_callback_checksum($request_body);
    $checksum_requested = $_SERVER['HTTP_QUICKPAY_CHECKSUM_SHA256'];

    if ($checksum_calculated !== $checksum_requested) {
      watchdog('quickpay', 'Transaction callback checksum didn\'t verify. Checksum calculated: @md5 Data: @data',
        array(
          '@data' => print_r($request, TRUE),
          '@md5' => $checksum_calculated,
        ), WATCHDOG_ERROR);
      return NULL;
    }

    $txn = new QuickpayTransaction($this);
    $txn->load_response($request);
    return $txn;
  }

  /**
   * Calculates the md5checksum for the callback request.
   *
   * @param $callback_data
   *  The data received by the callback page.
   *
   * @return
   *  The checksum.
   */
  protected function get_callback_checksum($callback_data) {
    return hash_hmac("sha256", $callback_data, $this->private_key);
  }

  /**
   * Returns a form for redirecting user to Quickpay for payment.
   *
   * @param string $order_id
   *   The order ID. This will typically be an ID from the system that is
   *   managing payments, e.g. a Commerce order ID.
   * @param string|int|float $amount
   *   The amount.
   * @param string $currency
   *   The ISO 4217 currency code.
   * @param string $module
   *   The name of the module managing this payment.
   * @param array $options
   *   An array of options. Properties include:
   *   - 'continue_url': The absolute URL that Quickpay should redirect the user
   *     to when the payment is complete.
   *   - 'cancel_url': The absolute URL that Quickpay should redirect the user
   *     to when the payment is cancelled.
   *   - 'msgtype': The type of authorization, either 'authorize', for regular
   *     payments, or 'subscribe' for subscription signups. Optional, defaults
   *     to 'authorize'.
   *   - 'description': Description of subscription, required for msgtype
   *     'subscribe' .
   *   - 'autocapture': Capture the payment immediately. Must only be used if
   *     the order is fulfilled immediately, for example if paying for access
   *     to some content that can then be viewed strait away.
   *   - 'splitpayment': Allow the transaction to be captured in multiple
   *     smaller parts. If not set, the can only be captured once.
   *   - 'autofee': Calculate and add the fee for the acquirer to the amount.
   *     This will increase the amount charged.
   *
   * @return array
   *   A from render array. The form will submit directly to Quickpay.
   */
  public function form($order_id, $amount, $currency, $module, $options) {
    $options += array(
      'autocapture' => FALSE,
      'msgtype' => 'authorize',
      'description' => NULL,
      'continue_url' => NULL,
      'cancel_url' => NULL,
    );

    if (empty($options['continue_url'])) {
      throw new QuickpayException(t("Missing required continue_url."));
    }

    if (empty($options['cancel_url'])) {
      throw new QuickpayException(t("Missing required cancel_url."));
    }

    if (!in_array($options['msgtype'], array('authorize', 'subscribe'))) {
      // FIXME: better error handling.
      throw new QuickpayException(t("Bad message type @type, must be 'authorize' or 'subscribe'.", array('@type' => $options['msgtype'])));
    }

    if ($options['msgtype'] == 'subscribe') {
      if (empty($options['description'])) {
        throw new QuickpayException(t("Missing required subscription description."));
      }
      if ($options['autocapture']) {
        throw new QuickpayException(t("Autocapture not possible for subscriptions."));
      }
    }

    if (!module_hook($module, 'quickpay_factory')) {
      throw new QuickpayException(t("%module doesn't implement hook_quickpay_factory.", array('%module' => $module)));
    }

    if (!module_hook($module, 'quickpay_callback')) {
      throw new QuickpayException(t("%module doesn't implement hook_quickpay_callback.", array('%module' => $module)));
    }

    $data = array();
    // Required variables.
    $data['version'] = QUICKPAY_VERSION_POST;
    $data['merchant_id'] = $this->merchant;
    $data['agreement_id'] = $this->agreement_id;
    $data['order_id'] = $this->order_prefix . $order_id;
    // Ensure that Order number is at least 4 characters. Else Quickpay will
    // reject the request.
    if (strlen($data['order_id']) < 4) {
      $data['order_id'] = $this->order_prefix . substr('0000' . $order_id,
                              -4+strlen($this->order_prefix));
    }

    $currency_info = Quickpay::currency_info($currency);
    $data['amount'] = Quickpay::wire_amount($amount, $currency_info);
    $data['currency'] = $currency_info['code'];
    $data['continueurl'] = $options['continue_url'];
    $data['cancelurl'] = $options['cancel_url'];
    $data['callbackurl'] = url(_quickpay_md5_path('quickpay/' . $order_id . '/' . $module), array('absolute' => TRUE));
    // End of required variables.

    // Use the user's current language
    if ($this->language == LANGUAGE_NONE) {
      global $language;
      $data['language'] = self::get_country_code($language->language);
    }
    else {
      $data['language'] = $this->language;
    }

    $data['autocapture'] = ($options['autocapture']) ? '1' : '0';

    // Build the checksum.
    $data['checksum'] = $this->get_checksum($data);

    if (module_exists('devel')) {
      dpm($data);
    }

    $form['#method'] = 'POST';
    $form['#action'] = 'https://payment.quickpay.net';

    foreach ($data as $name => $value) {
      $form[$name] = array('#type' => 'hidden', '#value' => $value);
    }

    $button_id = drupal_html_id('quicpay-submit-button');
    $form['submit'] = array(
      '#type' => 'submit',
      '#value' => t('Continue to QuickPay'),
      '#id' => $button_id,
    );

    return $form;
  }

  /**
   * Return Quickpay client.
   *
   * @return Quickpay\Quickpay
   *   The client.
   */
  public function client() {
    // todo: only load once.
    module_load_include('php', 'quickpay', 'lib/QuickPay/QuickPay');
    return new Quickpay\Quickpay(':' . $this->agreement_api_key);
  }

  /**
   * Calculate the md5checksum for the request.
   *
   * http://tech.quickpay.net/payments/hosted/#checksum
   *
   * @param array $data
   *   The data to POST to Quickpay.
   *
   * @return string
   *   The checksum.
   */
  protected function get_checksum(array $data) {
    $api_key = $this->agreement_api_key;
    ksort($data);
    $base = implode(" ", $data);
    return hash_hmac("sha256", $base, $api_key);
  }

  /**
   * This method currently doesn't work.
   *
   * Authorize a payment through the API. Only for test use.
   *
   * In order to handle credit card information, you'll either need a full PCI
   * certification which can cost 15000EUR or more, or have a signed
   * dispensation from Nets (which you're very unlikely to get), so this
   * function is prohibited for most sites. It is rather handy for unit
   * testing the API though.
   *
   * Just to be clear: using this function without PCI certification or
   * dispensation could mean that Nets shuts down your agreement without
   * notice.
   *
   * However, if you *are* allowed to use this function, please contact the
   * maintainer, so he knows it's in fact being used.
   */
  public function authorize($cardnumber, $expiration, $cvd, $order_id, $amount, $currency, $options = array()) {
    $currency_info = Quickpay::currency_info($currency);

    // Create payment.
    $data = array(
      'order_id' => $this->ordernumber($order_id),
      'currency' => $currency_info['code'],
    );
    $payment_res = $this->client()->request->post('/payments', $data);

    if (!$payment_res->is_success()) {
      Quickpay::log_error('Error creating payment.', $payment_res);
      throw new QuickpayException('Error creating payment.');
    }

    $payment = $payment_res->as_object();
    // Authorise the payment.
    $data = array(
      'amount' => Quickpay::wire_amount($amount, $currency_info),
      'card[number]' => $cardnumber,
      'card[expiration]' => $expiration,
      'card[cvd]' => $cvd,
      'auto_capture' => isset($options['autocapture']) && $options['autocapture'],
      'test_mode' => $this->test_mode,
      // Apparently Quickpay doesn't figure out the acquirer itself when using
      // the API anymore. As this is only for testing, we hardcore it.
      'acquirer' => 'clearhaus',
    );

    $authorize_res = $this->client()->request->post("/payments/{$payment->id}/authorize?synchronized", $data);

    if (!$authorize_res->is_success()) {
      Quickpay::log_error('Error authorizing payment.', $authorize_res);
      throw new QuickpayException('Error authorizing payment.');
    }

    $transaction = new QuickpayTransaction($this);
    $transaction->load_response($authorize_res->as_object());
    return $transaction;
  }

  /**
   * Subscribe test function. See authorize().
   */
  public function subscribe($cardnumber, $expiration, $cvd, $order_id, $description) {
    $request_data = array(
      'cardnumber' => $cardnumber,
      'expirationdate' => $expiration,
      'cvd' => $cvd,
      'ordernumber' => $this->ordernumber($order_id),
      'description' => $description,
    );
    $transaction = new QuickpayTransaction($this);
    // return $this->request('subscribe', $request_data, $transaction);
    throw new QuickpayException("Not implemented.");
  }

  /**
   * Ensure that ordernumber is properly formatted.
   */
  public function ordernumber($order_id) {
    $ordernumber = $this->order_prefix . $order_id;
    // Ensure that Order number is at least 4 characters.
    if (strlen($ordernumber) < 4) {
      $ordernumber = $this->order_prefix . substr('0000' . $order_id,
                     -4 + strlen($this->order_prefix));
    }
    return $ordernumber;
  }

  /**
   * Return the proper cardtypelock for the accepted cards.
   */
  protected function get_cardtypelock() {
    if (is_array($this->accepted_cards)) {
      $cards = $this->accepted_cards;
      // Aren't supported in cardtypelock.
      unset($cards['ikano']);
      return join(',', $cards);
    }
    // Already set to the proper string.
    return $this->accepted_cards;
  }

  /**
   * Returns an array of languages supported by Quickpay.
   */
  protected static function get_languages() {
    return array(
      'da' => t('Danish'),
      'de' => t('German'),
      'en' => t('English'),
      'fo' => t('Faeroese'),
      'fr' => t('French'),
      'gl' => t('Greenlandish'),
      'it' => t('Italian'),
      'no' => t('Norwegian'),
      'nl' => t('Dutch'),
      'pl' => t('Polish'),
      'se' => t('Swedish'),
    );
  }

  /**
   * Return the associated country code of a language code.
   *
   * @param string $langcode
   */
  protected static function get_country_code($langcode) {
    $langcodes = array(
      'da' => 'da',
      'de' => 'de',
      'en' => 'en',
      'fo' => 'fo',
      'fr' => 'fr',
      'kl' => 'gl',
      'it' => 'it',
      'nb' => 'no',
      'nn' => 'no',
      'nl' => 'nl',
      'pl' => 'pl',
      'sv' => 'se',
    );

    return isset($langcodes[$langcode]) ? $langcodes[$langcode] : 'en';
  }

  /**
   * Returns the settings form.
   *
   * @param array $settings
   *   Previously saved setting.
   */
  public static function settings_form($settings = array()) {
    $settings += array(
      'merchant' => '',
      'private_key' => '',
      'agreement_id' => '',
      'agreement_api_key' => '',
      'order_prefix' => '',
      'language' => LANGUAGE_NONE,
      'accepted_cards' => array('creditcard'),
      'test_mode' => FALSE,
      'debug' => FALSE,
      'popup' => FALSE,
      'splitpayment' => TRUE,
    );

    // Found in the "Integration" tab inside the QuickPay manager
    $form['merchant'] = array(
      '#type' => 'textfield',
      '#title' => t('Merchant ID'),
      '#description' => t('The Merchant ID as shown in the QuickPay admin.'),
      '#default_value' => $settings['merchant'],
      '#required' => TRUE,
    );

    $form['private_key'] = array(
      '#type' => 'textfield',
      '#title' => t('Private key'),
      '#description' => t('Your private key.'),
      '#default_value' => $settings['private_key'],
      '#required' => TRUE,
    );

    $form['agreement_id'] = array(
      '#type' => 'textfield',
      '#title' => t('Agreement ID'),
      '#description' => t('This is the Payment Window Agreement ID.'),
      '#default_value' => $settings['agreement_id'],
      '#required' => TRUE,
    );

    $form['agreement_api_key'] = array(
      '#type' => 'textfield',
      '#title' => t('API key'),
      '#description' => t('This is the Payment Window API key.'),
      '#default_value' => $settings['agreement_api_key'],
      '#required' => TRUE,
    );

    $form['order_prefix'] = array(
      '#type' => 'textfield',
      '#title' => t('Order id prefix'),
      '#description' => t('Prefix for order ids. Order ids must be uniqe when sent to QuickPay, use this to resolve clashes.'),
      '#default_value' => $settings['order_prefix'],
      '#element_validate' => array('quickpay_order_prefix_validate'),
    );

    $languages = self::get_languages() + array(LANGUAGE_NONE => t('Language of the user'));

    $form['language'] = array(
      '#type' => 'select',
      '#title' => t('Language'),
      '#description' => t('The language for the credit card form.'),
      '#options' => $languages,
      '#default_value' => $settings['language'],
    );

    $form['accepted_cards'] = array(
      '#type' => 'quickpay_payment_method',
      '#default_value' => $settings['accepted_cards'],
    );

    // todo: remove this and all usage.
    $form['splitpayment'] = array(
      '#type' => 'checkbox',
      '#title' => t('Split payments'),
      '#description' => t('Allows for capturing payments in parts.'),
      '#default_value' => $settings['splitpayment'],
    );

    $form['test_mode'] = array(
      '#type' => 'checkbox',
      '#title' => t('Test mode'),
      '#description' => t('When active, transactions will be run in test mode, even if the QuickPay account is in production mode. Order ids will get a T appended.'),
      '#default_value' => $settings['test_mode'],
    );

    $form['debug'] = array(
      '#type' => 'checkbox',
      '#title' => t('Debug log'),
      '#description' => t('Log all request and responses to QuickPay in watchdog.'),
      '#default_value' => $settings['debug'],
    );

    return $form;
  }

  /**
   * Get information about an currency.
   *
   * @param string $code
   *   The ISO 4217 currency code.
   *
   * @return NULL|array
   *   An array with the keys 'code' and 'multiplier', or null if not found.
   */
  public static function currency_info($code) {
    // @todo if commerce module is enabled, use its list.
    if (!array_key_exists($code, Quickpay::$currency_info)) {
      // Use a basic set.
      $base_currencies = array(
        'DKK' => array('code' => 'DKK', 'multiplier' => 100),
        'USD' => array('code' => 'USD', 'multiplier' => 100),
        'EUR' => array('code' => 'EUR', 'multiplier' => 100),
        'GBP' => array('code' => 'GBP', 'multiplier' => 100),
        'SEK' => array('code' => 'SEK', 'multiplier' => 100),
        'NOK' => array('code' => 'NOK', 'multiplier' => 100),
        'ISK' => array('code' => 'ISK', 'multiplier' => 100),
      );

      Quickpay::$currency_info += $base_currencies;
      // If still not found, throw an exception.
      if (!array_key_exists($code, Quickpay::$currency_info)) {
        throw new QuickpayException(t('Unknown currency code %currency', array('%currency' => $code)));
      }
    }
    return Quickpay::$currency_info[$code];
  }

  /**
   * Returns the amount adjusted by the multiplier for the currency.
   *
   * @param decimal $amount
   *   The amount.
   * @param array|string $currency_info
   *   An currency_info() array, or a currency code.
   */
  public static function wire_amount($amount, $currency_info) {
    if (!is_array($currency_info)) {
      $currency_info = Quickpay::currency_info($currency_info);
    }
    return (function_exists('bcmul') ?
        bcmul($amount, $currency_info['multiplier']) :
        $amount * $currency_info['multiplier']);

  }

  /**
   * Reverses wire_amount().
   *
   * @param int $amount
   *   The amount.
   * @param array|string $currency_info
   *   An currency_info() array, or a currency code.
   */
  public static function unwire_amount($amount, $currency_info) {
    if (!is_array($currency_info)) {
      $currency_info = Quickpay::currency_info($currency_info);
    }
    return (function_exists('bcdiv') ?
        bcdiv($amount, $currency_info['multiplier'], log10($currency_info['multiplier'])) :
        $amount / $currency_info['multiplier']);

  }

  /**
   * Log a client error.
   */
  public static function log_error($message, $response) {
    $error = $response->as_object();
    if (!empty($error->message)) {
      $message .= "\n" . $error->message;
    }
    if (!empty($error->errors)) {
      foreach ($error->errors as $key => $val) {
        $message .= "\n" . $key . ': ' . implode(', ', $val);
      }
    }

    //debug($message);
    // Bad errors doesn't return an object, but a string.
    //debug($response);
    // watchdog('quickpay', '@message', array('@message' => $message), WATCHDOG_ERROR);
  }
}

/**
 * Abstracts a transaction.
 */
class QuickpayTransaction {
  // State codes.
  const INITIAL = 0;
  const AUTHORIZED = 1;
  const CAPTURED = 3;
  const CANCELLED = 5;
  const REFUNDED = 7;
  const SUBSCRIBED = 9;

  protected $qp;
  protected $data;
  protected $loaded = FALSE;

  protected static $fields = array(
    'msgtype',
    'ordernumber',
    'amount',
    'currency',
    'time',
    'state',
    'qpstat',
    'qpstatmsg',
    'chstat',
    'chstatmsg',
    'merchant',
    'merchantemail',
    'transaction',
    'cardtype',
    'cardnumber',
    'cardexpire',
    'splitpayment',
    'fraudprobability',
    'fraudremarks',
    'fraudreport',
    'fee',
    'md5check',
    'history',
  );

  /**
   * Create a transaction object.
   */
  public function __construct(Quickpay $qp, $txnid = NULL) {
    $this->qp = $qp;
    $this->data = array();
    if (preg_match('/^a:/', $txnid)) {
      list($this->data['id'], $this->data['currency']) = unserialize($txnid);
    }
    else {
      $this->data['id'] = $txnid;
    }
  }

  /**
   * Magic method.
   */
  public function __get($field) {
    if (!$this->loaded) {
      $this->status();
    }

    if (in_array($field, array('currency', 'amount', 'history'))) {
      return $this->{$field}();
    }

    // Legacy. Should have made an abstracted message property/method earlier,
    // but that's too late.
    if ($field == 'qpstatmsg') {
      $this->last_message();
    }

    if (array_key_exists($field, $this->data)) {
      return $this->data[$field];
    }

    throw new QuickpayException(t("Unknown property @name", array('@name' => $field)));
  }

  /**
   * Magic method.
   */
  public function __isset($field) {
    if ($field != 'transaction' && !$this->loaded) {
      $this->status();
    }
    if (isset($this->data[$field])) {
      return $this->data[$field];
    }
  }

  /**
   * Get last transaction operation.
   */
  protected function get_last_operation() {
    if (!$this->loaded) {
      $this->status();
    }
    if (isset($this->data['operations'])) {
      $operations = $this->data['operations'];
      return array_pop($operations);
    }

    return NULL;
  }

  /**
   * Get last operation message.
   */
  public function last_message() {
    if ($operation = $this->get_last_operation()) {
      return $operation->qp_status_msg;
    }

    return '';
  }

  /**
   * Get last operation state.
   */
  public function last_state() {
    if ($operation = $this->get_last_operation()) {
      return $operation->type;
    }

    return '';
  }

  /**
   * Returns the currency for this transaction.
   */
  public function currency() {
    if (!$this->loaded) {
      $this->status();
    }
    // @todo call status command if not set.
    if ($this->data['currency']) {
      return $this->data['currency'];
    }
    throw new QuickpayException(t('No currency defined for transaction @txn_id', array('@txn_id' => $this->data['transaction'])));
  }

  /**
   * Returns the amount for this transaction.
   */
  public function amount() {
    if (!$this->loaded) {
      $this->status();
    }
    if ($this->data['amount']) {
      return Quickpay::unwire_amount($this->data['amount'], $this->currency());
    }
    throw new QuickpayException(t('No amount for transaction @txn_id', array('@txn_id', $this->data['transaction'])));
  }

  /**
   * Returns the history for this transaction.
   */
  public function history() {
    if (!$this->loaded || !isset($this->history)) {
      $this->status();
    }
    if ($this->data['history']) {
      return $this->data['history'];
    }
    return array();
  }

  /**
   * Return the balance for this transaction.
   *
   * This is (for histotical reasons) how much is still left to be captured of
   * the authorized amount.
   */
  public function balance() {
    // The v10 API keeps track of the balance, but as how much has been
    // captured. So we find the authorization amount and subtracts the balance
    // from that.
    if (isset($this->data['operations'])) {
      foreach ($this->data['operations'] as $operation) {
        if ($operation->type == 'authorize') {
          $balance = $operation->amount;
          break;
        }
      }

      if ($balance) {
        return Quickpay::unwire_amount($balance - $this->data['balance'], $this->currency());
      }
    }
  }

  /**
   * Extract named object properties into an associative array.
   */
  public function extract() {
    // Safety, don't attempt to extract on empty transactions.
    if (empty($this->data['transaction'])) {
      throw new QuickpayException("No transaction id.");
    }
    // PHP 5.2 can't pass the result of func_get_args() directly to another
    // function, so we use a variable.
    $args = func_get_args();
    return array_intersect_key($this->data, array_flip($args));
  }

  /**
   * Returns the amount adjusted by the multiplier for the currency of this
   * transaction.
   */
  public function wire_amount($amount = NULL) {
    if (!$amount) {
      return $this->data['amount'];
    }
    return Quickpay::wire_amount($amount, $this->currency());
  }

  /**
   * Load a request response.
   */
  public function load_response($response) {
    if (is_object($response)) {
      $response = (array) $response;
    }
    if (!is_array($response)) {
      throw new QuickpayException(t('Transaction could not be loaded from response: !response', array(
        '!response', print_r($response, TRUE),
      )));
    }

    $this->data = $response;
    $this->loaded = TRUE;
  }

  /**
   * Encode the minimal needed information about this transaction, for later
   * loading. This is what modules using Quickpay should use as a reference.
   */
  public function store() {
    if ($this->data['currency']) {
      return serialize(array($this->data['id'], $this->data['currency']));
    }
    return $this->transaction;
  }

  /**
   * Capture on an authorised transaction.
   */
  public function capture($amount, $finalize = FALSE) {
    // Capture payment.
    $data = array(
      'amount' => $this->wire_amount($amount),
      // Finalize no longer supported.
    );
    $capture_res = $this->qp->client()->request->post("/payments/{$this->data['id']}/capture?synchronized", $data);

    if (!$capture_res->is_success()) {
      Quickpay::log_error('Error capturing payment.', $capture_res);
      throw new QuickpayException('Error capturing payment.');
    }

    $this->load_response($capture_res->as_object());
  }

  /**
   * Update this transaction with status information from Quickpay.
   */
  public function status() {
    $payment_res = $this->qp->client()->request->get("/payments/{$this->data['id']}");

    if (!$payment_res->is_success()) {
      Quickpay::log_error('Error fetching payment.', $payment_res);
      throw new QuickpayException('Error fetching payment.');
    }

    $this->load_response($payment_res->as_object());
  }

  /**
   * Renew an authorisation.
   */
  public function renew() {
    $renew_res = $this->qp->client()->request->post("/payments/{$this->data['id']}/renew?synchronized");

    if (!$renew_res->is_success()) {
      Quickpay::log_error('Error renewing payment.', $renew_res);
      throw new QuickpayException('Error renewing payment.');
    }

    $this->load_response($renew_res->as_object());
  }

  /**
   * Cancel an authorised transaction.
   */
  public function cancel() {
    $cancel_res = $this->qp->client()->request->post("/payments/{$this->data['id']}/cancel?synchronized");

    if (!$cancel_res->is_success()) {
      Quickpay::log_error('Error cancelling payment.', $cancel_res);
      throw new QuickpayException('Error cancelling payment.');
    }

    $this->load_response($cancel_res->as_object());
  }

  /**
   * Refund previously captured money.
   */
  public function refund($amount) {
    $data = array(
      'amount' => $this->wire_amount($amount),
    );
    $refund_res = $this->qp->client()->request->post("/payments/{$this->data['id']}/refund?synchronized", $data);

    if (!$refund_res->is_success()) {
      Quickpay::log_error('Error refunding payment.', $refund_res);
      throw new QuickpayException('Error refunding payment.');
    }

    $this->load_response($refund_res->as_object());
  }

  /**
   * Create a new payment on a subscription transaction.
   */
  function recurring($order_id, $amount, $currency, $options = array()) {
    $options += array(
      'autocapture' => FALSE,
    );
    $currency_info = Quickpay::currency_info($currency);
    $request_data = array(
      'transaction' => $this->data['transaction'],
      'ordernumber' => $this->qp->ordernumber($order_id),
      'amount' => Quickpay::wire_amount($amount, $currency_info),
      'currency' => $currency_info['code'],
      'autocapture' => $options['autocapture'] ? '1' : '0',
    );
    $transaction = new QuickpayTransaction($this->qp);
    // return $this->qp->request('recurring', $request_data, $transaction);
    throw new QuickpayException("Not implemented.");

  }

  /**
   * Whether this transaction was a success.
   */
  public function success() {
    if ($operation = $this->get_last_operation()) {
      if ($operation->qp_status_code == '20000') {
        return TRUE;
      }
    }

    return FALSE;
  }
}
