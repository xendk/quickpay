<?php
/**
 * @file
 * Quickpay and QuickpayTransaction classes.
 */

class QuickpayException extends Exception {}

class Quickpay {
  private static $currency_info = array();
  private $test_mode = FALSE;
  private $debug = FALSE;
  private $merchant;
  private $secret;
  private $order_prefix = '';
  private $accepted_cards = array('creditcard');
  // private $auto_fee
  private $popup = FALSE;
  // private $link_message // needed? shouldn't the calling module create that page?
  private $language = LANGUAGE_NONE; // With a 'use user language' default option.

  public function __construct($options) {
    // Check options, throw exception if problems
    if (empty($options['merchant']) || !preg_match('/^[0-9]{8}$/', $options['merchant'])) {
      throw new QuickpayException(t('Invalid merchant number'));
    }
    $this->merchant = $options['merchant'];
    if (empty($options['secret'])) {
      throw new QuickpayException(t('Empty secret?'));
    }
    $this->secret = $options['secret'];
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

  /* public static function transaction($options, $txnid) { */
  /*   $qp = new Quickpay($options); */
  /*   return $qp->transaction($txnid); */
  /* } */

  public function transaction($txnid) {
    return new QuickpayTransaction($this, $txnid);
  }

  public function transaction_callback() {
   static $md5_order = array(
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
    'fraudprobability',
    'fraudremarks',
    'fraudreport',
    'fee',
  );

  // Check that it validates.
  $md5_string = "";
  foreach ($md5_order as $field) {
    if (isset($_POST[$field])) {
      $md5_string .= $_POST[$field];
    }
  }
  if (md5($md5_string . $this->secret) != $_POST['md5check']) {
    watchdog('quickpay', 'Transaction callback md5 didn\'t verify. MD5 calculated: @md5 Data: @data',
      array('@data' => print_r($_POST, TRUE), '@md5' => md5($md5_string . $this->secret)), WATCHDOG_ERROR);
    return NULL;
  }

  $txn = new QuickpayTransaction($this);
  $txn->load_response($_POST);
  return $txn;
 }

  /**
   * Returns a form for redirecting user to Quickpay for payment.
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

    // Required variables.
    $data['protocol'] = QUICKPAY_VERSION_POST;
    $data['msgtype'] = $options['msgtype'];
    $data['merchant'] = $this->merchant;
    // FIXME: work together with i18n/locale
    if ($this->language == LANGUAGE_NONE) {
      // @todo find users current language
      $data['language'] = 'da';
    }
    else {
      $data['language'] = $this->language;
    }
    $data['ordernumber'] = $this->order_prefix . $order_id;
    // Ensure that Order number is at least 4 characters. Else Quickpay will
    // reject the request.
    if (strlen($data['ordernumber']) < 4) {
      $data['ordernumber'] = $this->order_prefix . substr('0000' . $order_id,
                             -4+strlen($this->order_prefix));
    }

    $currency_info = Quickpay::currency_info($currency);
    $data['amount'] = Quickpay::wire_amount($amount, $currency_info);
    $data['currency'] = $currency_info['code'];
    $data['continueurl'] = $options['continue_url'];
    $data['cancelurl'] = $options['cancel_url'];

    // End of required variables.
    $data['callbackurl'] = url(_quickpay_md5_path('quickpay/' . $order_id . '/' . $module), array('absolute' => TRUE));

    $data['autocapture'] = ($options['autocapture']) ? '1' : '0';

    if (!empty($options['description'])) {
      $data['description'] = $options['description'];
    }

    if ($this->test_mode) {
      $data['testmode'] = '1';
      $data['ordernumber'] .= "T";
    }
    else {
      $data['testmode'] = '0';
    }

    if (!empty($options['autofee']) && $options['autofee']) {
      $data['autofee'] = 1;
    }

    $data['cardtypelock'] = $this->get_cardtypelock();

    // Other possible variables:
    // $data['CUSTOM_*'];

    $data['md5check'] = $this->md5checksum($data);

    $form['#method'] = 'POST';
    $form['#action'] = 'https://secure.quickpay.dk/form/';

    foreach ($data as $name => $value) {
      $form[$name] = array('#type' => 'hidden', '#value' => $value);
    }

    $button_id = drupal_html_id('quicpay-submit-button');
    $form['submit'] = array(
      '#type' => 'submit',
      '#value' => t('Continue to QuickPay'),
      '#id' => $button_id,
    );

    // Create a popup, if so instructed
    // Not supported, for the moment being.
/*     if ($this->popup) { */
/*     // The oddity of setting the return urls by JavaScript, ensures that */
/*     // we're only using the JavaScript requiring popdown page if JavaScript is */
/*     // enabled. */

/*     // Replace continueurl and cancelurl with a md5 protected call to */
/*     // quickpay_popdown. */
/*     $data['continueurl'] = url(_quickpay_md5_path('quickpay_popdown/' . rawurlencode($data['continueurl'])), array( 'absolute' => TRUE)); */
/*     $data['cancelurl'] = url(_quickpay_md5_path('quickpay_popdown/' . rawurlencode($data['cancelurl'])), array('absolute' => TRUE)); */

/*     $data['md5check'] = $this->md5checksum($data); */
/*     // Interpolation of arrays in strings is iffy. Lets just use regular scalars. */
/*     $continueurl = $data['continueurl']; */
/*     $cancelurl = $data['cancelurl']; */
/*     $md5check = $data['md5check']; */
/*       $js = <<<EOF */
/*         jQuery(document).ready(function() { */
/*             console.dir(jQuery('#$button_id').closest('form')); */
/*             jQuery('#$button_id').closest('form').submit(function() { */
/*                 var left = (screen.width) ? (screen.width-670)/2 : 0; */
/*                 var top = (screen.height) ? (screen.height-500)/2 : 0; */
/*                 jQuery(this).find('#edit-continueurl').val('$continueurl'); */
/*                 jQuery(this).find('#edit-cancelurl').val('$cancelurl'); */
/*                 jQuery(this).find('#edit-md5check').val('$md5check'); */
/*                 window.open('','quickpay_payment', 'top=' + top + ',left=' + left + ',height=500,width=670,scrollbars=yes,toolbars=no,statusbar=yes,location=0'); */
/*                 jquery(this).attr('target', 'quickpay_payment'); */
/*                 return false; */
/*               }); */
/*           }); */
/* EOF; */
/*       $form['submit']['#attached'] = array( */
/*         'js' => array( */
/*           $js => array('type' => 'inline'), */
/*         ), */
/*       ); */
/*     } */
    return $form;
  }

  public function request($type, $request_data, QuickpayTransaction $transaction) {
    if (!is_array($request_data)) {
      $request_data = (array) $request_data;
    }

    $request_data['protocol'] = QUICKPAY_VERSION_API;
    $request_data['msgtype'] = $type;
    $request_data['merchant'] = $this->merchant;
    $request_data['md5check'] = $this->md5checksum($request_data);

    if ($this->debug) {
      debug($request_data, 'Quickpay request parameters');
    }

    if (!$this->validate_request($request_data)) {
      throw new QuickpayException(t("Request message didn\'t pass validation."));
    }
    $request_options = array(
      'headers' => array('Content-Type' => 'application/x-www-form-urlencoded'),
      'method' => 'POST',
      'data' => http_build_query($request_data, FALSE, '&'),
      'max_redirects' => 0,
    );
    /**
     * We're using curl as drupal_http_request cannot be used in unit tests.
     */
    $ch = curl_init();
    curl_setopt($ch, CURLOPT_URL, 'https://secure.quickpay.dk/api');
    curl_setopt($ch, CURLOPT_POST, count($request_data));
    curl_setopt($ch, CURLOPT_POSTFIELDS, http_build_query($request_data, FALSE, '&'));
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    $response = curl_exec($ch);


    if ($this->debug) {
      debug($response, 'Quickpay response');
    }

    if (!$response) {
      throw new QuickpayException(t('Server returned non-success code or empty result'));
    }
    $transaction->load_response($this->response($response));

    return $transaction;
  }

  function md5checksum($data) {
    $fields = array(
      'protocol',
      'msgtype',
      'merchant',
      'language',
      'ordernumber',
      'amount',
      'finalize',
      'currency',
      'continueurl',
      'cancelurl',
      'callbackurl',
      'autocapture',
      'cardnumber',
      'expirationdate',
      'cvd',
      // 'autofee',
      'cardtypelock',
      //
      'ipaddress',
      'transaction',
      'description',
      'testmode',
      'splitpayment',
    );

    $string = "";
    $t = array();
    foreach ($fields as $field) {
      if (isset($data[$field])) {
        $string .= $data[$field];
        $t[] = $field;
      }
    }
    /* print_r($string . $this->secret);die(); */
    return md5($string . $this->secret);
  }

  /**
   * Validates that the request fields is formatted as expected by Quickpay.
   *
   * Throws an exception on failure.
   *
   * @param $data Associative array of params.
   *
   */
  protected function validate_request($data) {
    $fields =
      array(
        'protocol' => '/^(' . QUICKPAY_VERSION_API . '|' . QUICKPAY_VERSION_POST. ')$/',
        'msgtype' => '/^[a-z]+$/',
        'merchant' => '/^[0-9]{8}$/',
        'ordernumber' => '/^[a-zA-Z0-9]{4,20}$/',
        'amount' => '/^[0-9]{1,10}$/',
        'finalize' => '/^[0-1]{1}$/',
        'currency' => '/^[A-Z]{3}$/',
        'autocapture' => '/^[0-1]{1}$/',
        'cardnumber' => '/^[0-9]{13,19}$/',
        'expirationdate' => '/^[0-9]{4}$/',
        'cvd' => '/^[0-9]{0,4}$/',
        'cardtypelock' => '/^[a-zA-Z,]{0,128}$/',
        'splitpayment' => '/^[0-1]{1}$/',
        'transaction' => '/^[0-9]{1,32}$/',
        'description' => '/^[\w _\-\.]{0,20}$/',
        'md5check' => '/^[a-z0-9]{32}$/',
        'CUSTOM_' => '/^[\w _\-\.]{0,20}$/'
      );

    foreach ($data as $field => $value) {
      // No NULL values please
      if (is_null($value)) {
        throw new QuickpayException(t('%field cannot be NULL', array('%field' => $field)));
      }
      elseif ($fields[$field]) {
        if (!preg_match($fields[$field], $value)) {
          // We're not logging the actual value, as that might be
          // sensitive information
          throw new QuickpayException(t('%field didn\'t pass validation.',
              array('%field' => $field)));
        }
      }
      elseif (preg_match('/^CUSTOM_/', $field)) {
        if (!preg_match($fields['CUSTOM_'], $value)) {
          throw new QuickpayException(t('%field didn\'t pass validation.',
              array('%field' => $field)));
        }
      }
      else {
        throw new QuickpayException(t('Unknown field %field.',
            array('%field' => $field)));
      }
    }
    return TRUE;
  }

  /**
   * Parses the XML response from Quickpay into an associative
   * array. Internal use only.
   *
   * @param string $response the XML response.
   * @return array associative array
   */
  protected function response($response) {
    // Load XML in response into DOM
    $result = array();
    $dom = new DOMDocument;
    $dom->loadXML($response);
    // Find elements en response and put them in an associative array
    $xpath = new DOMXPath($dom);
    $elements = $xpath->query('/response/*');
    foreach ($elements as $cn) {
      // If the element has (real) children - this is the case for status->history and chstatus->entry
      if ($cn->childNodes->length > 1) {
        $child = array();
        foreach ($cn->childNodes as $hn) {
          $child[$hn->nodeName] = $hn->nodeValue;
        }
        $result[$cn->nodeName][] = $child;
      }
      else {
        $result[$cn->nodeName] = $cn->nodeValue;
      }
    }

    return $result;
  }


  /**
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
    $options += array(
      'autocapture' => FALSE,
      'splitpayment' => FALSE,
    );
    $currency_info = Quickpay::currency_info($currency);
    $request_data = array(
      'cardnumber' => $cardnumber,
      'expirationdate' => $expiration,
      'cvd' => $cvd,
      'ordernumber' => $this->ordernumber($order_id),
      'amount' => Quickpay::wire_amount($amount, $currency_info),
      'currency' => $currency_info['code'],
      'autocapture' => $options['autocapture'] ? '1' : '0',
      'splitpayment' => $options['splitpayment'] ? '1' : '0',
    );
    $transaction = new QuickpayTransaction($this);
    return $this->request('authorize', $request_data, $transaction);
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
    return $this->request('subscribe', $request_data, $transaction);
  }

  public function ordernumber($order_id) {
    $ordernumber = $this->order_prefix . $order_id;
    // Ensure that Order number is at least 4 characters.
    if (strlen($ordernumber) < 4) {
      $ordernumber = $this->order_prefix . substr('0000' . $order_id,
                     -4+strlen($this->order_prefix));
    }
    return $ordernumber;
  }

  /**
   * Return the proper cardtypelock for the accepted cards.
   */
  protected function get_cardtypelock() {
    if (is_array($this->accepted_cards)) {
      $cards = $this->accepted_cards;
      unset($cards['ikano']); // Aren't supported in cardtypelock.
      return join(',', $cards);
    }
    // Already set to the proper string.
    return $this->accepted_cards;
  }

  public static function settings_form($settings = array()) {
    $settings += array(
      'merchant' => '',
      'secret' => '',
      'order_prefix' => '',
      'language' => LANGUAGE_NONE,
      'accepted_cards' => array('creditcard'),
      'test_mode' => FALSE,
      'debug' => FALSE,
      'popup' => FALSE,
    );

    $form['merchant'] = array(
      '#type' => 'textfield',
      '#title' => t('Merchant number'),
      '#description' => t('Merchant id as shown in the QuickPay admin. NOTE: <em>not</em> the same as PBS id'),
      '#default_value' => $settings['merchant'],
      '#required' => TRUE,
    );

    $form['secret'] = array(
      '#type' => 'textfield',
      '#title' => t('MD5 secret'),
      '#description' => t('MD5 secret as shown in the Quickpay admin.'),
      '#default_value' => $settings['secret'],
      '#required' => TRUE,
    );

    $form['order_prefix'] = array(
      '#type' => 'textfield',
      '#title' => t('Order id prefix'),
      '#description' => t('Prefix for order ids. Order ids must be uniqe when sent to QuickPay, use this to resolve clashes.'),
      '#default_value' => $settings['order_prefix'],
      '#element_validate' => array('quickpay_order_prefix_validate'),
    );

    $languages = array(
      LANGUAGE_NONE => t('Language of the user'),
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

    // FIXME: work together with i18n
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

    /* $form['popup'] = array( */
    /*   '#type' => 'checkbox', */
    /*   '#title' => t('Use popup'), */
    /*   '#description' => t('Create a popup window instead of redirecting to the payment form.'), */
    /*   '#default_value' => $settings['popup'], */
    /* ); */

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
   * @param $code the ISO 4217 currency code.
   *
   * @return NULL or an array with the keys 'code' and 'multiplier'.
   */
  public static function currency_info($code) {
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
   * @param $amount the amount.
   * @param $currency_info an currency_info() array, or a currency code.
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
   * @param $amount the amount.
   * @param $currency_info an currency_info() array, or a currency code.
   */
  public static function unwire_amount($amount, $currency_info) {
    if (!is_array($currency_info)) {
      $currency_info = Quickpay::currency_info($currency_info);
    }
    return (function_exists('bcdiv') ?
        bcdiv($amount, $currency_info['multiplier']) :
        $amount / $currency_info['multiplier']);

  }
}

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

  public function __construct(Quickpay $qp, $txnid = NULL) {
    $this->qp = $qp;
    foreach (QuickpayTransaction::$fields as $field) {
      $this->data[$field] = NULL;
    }
    if (preg_match('/^a:/', $txnid)) {
      list($this->data['transaction'], $this->data['currency']) = unserialize($txnid);
    }
    else {
      $this->data['transaction'] = $txnid;
    }
  }

  public function __get($field) {
    if (!$this->loaded) {
      $this->status();
    }
    if ($field == 'currency' || $field == 'amount' || $field == 'history') {
      return $this->{$field}();
    }
    if (array_key_exists($field, $this->data)) {
      return $this->data[$field];
    }
    throw new QuickpayException(t("Unknown property @name", array('@name' => $field)));
  }

  public function __isset($field) {
    if ($field != 'transaction' && !$this->loaded) {
      $this->status();
    }
    if (isset($this->data[$field])) {
      return $this->data[$field];
    }

  }

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

  public function amount() {
    if (!$this->loaded) {
      $this->status();
    }
    if ($this->data['amount']) {
      return Quickpay::unwire_amount($this->data['amount'], $this->currency());
    }
    throw new QuickpayException(t('No amount for transaction @txn_id', array('@txn_id', $this->data['transaction'])));
  }

  public function history() {
    if (!$this->loaded || !isset($this->history)) {
      $this->status();
    }
    if ($this->data['history']) {
      return $this->data['history'];
    }
    return array();
  }

  public function extract() {
    // Safety, don't attempt to extract on empty transactions.
    if (empty($this->data['transaction'])) {
      throw new QuickpayException("No transaction id.");
    }
    return array_intersect_key($this->data, array_flip(func_get_args()));
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

  public function load_response(array $response) {
    if (!isset($response['qpstat'])) {
      throw new QuickpayException(t('No state from Quickpay'));
    }
    $success = $response['qpstat'] == '000';
    foreach ($this->data as $field => $dummy) {
      if (isset($response[$field])) {
        // Only overwrite with empty values when the request was a
        // success. Quickpay returns empty fields on failure.
        if ($success || !empty($response[$field])){
          $this->data[$field] = $response[$field];
        }
      }
    }
    $this->loaded = TRUE;
  }

  public function store() {
    if ($this->data['currency']){
      return serialize(array($this->data['transaction'], $this->data['currency']));
    }
    return $this->transaction;
  }

  public function capture($amount, $finalize = FALSE) {
    $this->qp->request('capture', $this->extract('transaction') + array('amount' => $this->wire_amount($amount), 'finalize' => $finalize ? '1' : '0'), $this);
  }

  public function status() {
    $this->qp->request('status', $this->extract('transaction'), $this);
    // Throw an exception if Quickpay doesn't know this transaction.
    // Perhaps this is better?
    /* if (!$this->data['qpstatmsg'] == 'Transaction not found') { */
    if (!$this->success()) {
      throw new QuickpayException(t("Status error."));
    }
  }

  public function renew() {
    $this->qp->request('renew', $this->extract('transaction'), $this);
  }

  public function cancel() {
    $this->qp->request('cancel', $this->extract('transaction'), $this);
  }

  public function refund($amount) {
    $this->qp->request('refund', $this->extract('transaction') + array('amount' => $this->wire_amount($amount)), $this);
  }

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
    return $this->qp->request('recurring', $request_data, $transaction);
  }

  /**
   * Whether this transaction was a success.
   */
  public function success() {
    if (!isset($this->data['qpstat'])) {
      return FALSE;
    }
    return $this->data['qpstat'] == '000';
  }
}